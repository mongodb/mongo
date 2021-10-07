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

/*
 * Home directory initialize command: create the directory if it doesn't exist, else remove
 * everything except the RNG log file.
 *
 * Redirect the "cd" command to /dev/null so chatty cd implementations don't add the new working
 * directory to our output.
 */
#define FORMAT_HOME_INIT_CMD   \
    "test -e %s || mkdir %s; " \
    "cd %s > /dev/null && rm -rf `ls | sed /CONFIG.rand/d`"

/*
 * compressor --
 *     Configure compression.
 */
static const char *
compressor(uint32_t compress_flag)
{
    const char *p;

    p = "unrecognized compressor flag";
    switch (compress_flag) {
    case COMPRESS_NONE:
        p = "none";
        break;
    case COMPRESS_LZ4:
        p = "lz4";
        break;
    case COMPRESS_SNAPPY:
        p = "snappy";
        break;
    case COMPRESS_ZLIB:
        p = "zlib";
        break;
    case COMPRESS_ZSTD:
        p = "zstd";
        break;
    default:
        testutil_die(EINVAL, "illegal compression flag: %#" PRIx32, compress_flag);
        /* NOTREACHED */
    }
    return (p);
}

/*
 * encryptor --
 *     Configure encryption.
 */
static const char *
encryptor(uint32_t encrypt_flag)
{
    const char *p;

    p = "unrecognized encryptor flag";
    switch (encrypt_flag) {
    case ENCRYPT_NONE:
        p = "none";
        break;
    case ENCRYPT_ROTN_7:
        p = "rotn,keyid=7";
        break;
    case ENCRYPT_SODIUM:
        p = "sodium,secretkey=" SODIUM_TESTKEY;
        break;
    default:
        testutil_die(EINVAL, "illegal encryption flag: %#" PRIx32, encrypt_flag);
        /* NOTREACHED */
    }
    return (p);
}

/*
 * encryptor_at_open --
 *     Configure encryption for wts_open().
 *
 * This must set any secretkey. When keyids are in use it can return NULL.
 */
static const char *
encryptor_at_open(uint32_t encrypt_flag)
{
    const char *p;

    p = NULL;
    switch (encrypt_flag) {
    case ENCRYPT_NONE:
        break;
    case ENCRYPT_ROTN_7:
        break;
    case ENCRYPT_SODIUM:
        p = "sodium,secretkey=" SODIUM_TESTKEY;
        break;
    default:
        testutil_die(EINVAL, "illegal encryption flag: %#" PRIx32, encrypt_flag);
        /* NOTREACHED */
    }
    return (p);
}

static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    WT_DECL_RET;
    int nw;

    (void)(handler);
    (void)(session);

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

static int
handle_progress(
  WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *operation, uint64_t progress)
{
    (void)(handler);
    (void)(session);

    track(operation, progress, NULL);
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
 * create_database --
 *     Create a WiredTiger database.
 */
void
create_database(const char *home, WT_CONNECTION **connp)
{
    WT_CONNECTION *conn;
    size_t max;
    char config[8 * 1024], *p;
    const char *enc;

    p = config;
    max = sizeof(config);

    CONFIG_APPEND(p,
      "create=true"
      ",cache_size=%" PRIu32
      "MB"
      ",checkpoint_sync=false"
      ",error_prefix=\"%s\""
      ",operation_timeout_ms=2000",
      g.c_cache, progname);

    /* In-memory configuration. */
    if (g.c_in_memory != 0)
        CONFIG_APPEND(p, ",in_memory=1");

    /* LSM configuration. */
    if (DATASOURCE("lsm"))
        CONFIG_APPEND(p, ",lsm_manager=(worker_thread_max=%" PRIu32 "),", g.c_lsm_worker_threads);

    if (DATASOURCE("lsm") || g.c_cache < 20)
        CONFIG_APPEND(p, ",eviction_dirty_trigger=95");

    /* Eviction worker configuration. */
    if (g.c_evict_max != 0)
        CONFIG_APPEND(p, ",eviction=(threads_max=%" PRIu32 ")", g.c_evict_max);

    /* Logging configuration. */
    if (g.c_logging)
        CONFIG_APPEND(p,
          ",log=(enabled=true,archive=%d,prealloc=%d,file_max=%" PRIu32 ",compressor=\"%s\")",
          g.c_logging_archive ? 1 : 0, g.c_logging_prealloc ? 1 : 0, KILOBYTE(g.c_logging_file_max),
          compressor(g.c_logging_compression_flag));

    /* Encryption. */
    if (g.c_encryption) {
        enc = encryptor(g.c_encryption_flag);
        if (enc != NULL)
            CONFIG_APPEND(p, ",encryption=(name=%s)", enc);
    }

/* Miscellaneous. */
#ifdef HAVE_POSIX_MEMALIGN
    CONFIG_APPEND(p, ",buffer_alignment=512");
#endif

    if (g.c_mmap)
        CONFIG_APPEND(p, ",mmap=1");
    if (g.c_mmap_all)
        CONFIG_APPEND(p, ",mmap_all=1");

    if (g.c_direct_io)
        CONFIG_APPEND(p, ",direct_io=(data)");

    if (g.c_data_extend)
        CONFIG_APPEND(p, ",file_extend=(data=8MB)");

    /*
     * Run the statistics server and/or maintain statistics in the engine. Sometimes specify a set
     * of sources just to exercise that code.
     */
    if (g.c_statistics_server) {
        if (mmrand(NULL, 0, 5) == 1 && memcmp(g.uri, "file:", strlen("file:")) == 0)
            CONFIG_APPEND(
              p, ",statistics=(fast),statistics_log=(json,on_close,wait=5,sources=(\"file:\"))");
        else
            CONFIG_APPEND(p, ",statistics=(fast),statistics_log=(json,on_close,wait=5)");
    } else
        CONFIG_APPEND(p, ",statistics=(%s)", g.c_statistics ? "fast" : "none");

    /* Optionally stress operations. */
    CONFIG_APPEND(p, ",timing_stress_for_test=[");
    if (g.c_timing_stress_aggressive_sweep)
        CONFIG_APPEND(p, ",aggressive_sweep");
    if (g.c_timing_stress_checkpoint)
        CONFIG_APPEND(p, ",checkpoint_slow");
    if (g.c_timing_stress_checkpoint_prepare)
        CONFIG_APPEND(p, ",prepare_checkpoint_delay");
    if (g.c_timing_stress_checkpoint_reserved_txnid_delay)
        CONFIG_APPEND(p, ",checkpoint_reserved_txnid_delay");
    if (g.c_timing_stress_failpoint_hs_delete_key_from_ts)
        CONFIG_APPEND(p, ",failpoint_history_store_delete_key_from_ts");
    if (g.c_timing_stress_failpoint_hs_insert_1)
        CONFIG_APPEND(p, ",failpoint_history_store_insert_1");
    if (g.c_timing_stress_failpoint_hs_insert_2)
        CONFIG_APPEND(p, ",failpoint_history_store_insert_2");
    if (g.c_timing_stress_hs_checkpoint_delay)
        CONFIG_APPEND(p, ",history_store_checkpoint_delay");
    if (g.c_timing_stress_hs_search)
        CONFIG_APPEND(p, ",history_store_search");
    if (g.c_timing_stress_hs_sweep)
        CONFIG_APPEND(p, ",history_store_sweep_race");
    if (g.c_timing_stress_split_1)
        CONFIG_APPEND(p, ",split_1");
    if (g.c_timing_stress_split_2)
        CONFIG_APPEND(p, ",split_2");
    if (g.c_timing_stress_split_3)
        CONFIG_APPEND(p, ",split_3");
    if (g.c_timing_stress_split_4)
        CONFIG_APPEND(p, ",split_4");
    if (g.c_timing_stress_split_5)
        CONFIG_APPEND(p, ",split_5");
    if (g.c_timing_stress_split_6)
        CONFIG_APPEND(p, ",split_6");
    if (g.c_timing_stress_split_7)
        CONFIG_APPEND(p, ",split_7");
    CONFIG_APPEND(p, "]");

    /* Extensions. */
    CONFIG_APPEND(p, ",extensions=[\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\"],",
      g.c_reverse ? REVERSE_PATH : "", access(LZ4_PATH, R_OK) == 0 ? LZ4_PATH : "",
      access(ROTN_PATH, R_OK) == 0 ? ROTN_PATH : "",
      access(SNAPPY_PATH, R_OK) == 0 ? SNAPPY_PATH : "",
      access(ZLIB_PATH, R_OK) == 0 ? ZLIB_PATH : "", access(ZSTD_PATH, R_OK) == 0 ? ZSTD_PATH : "",
      access(SODIUM_PATH, R_OK) == 0 ? SODIUM_PATH : "");

    /*
     * Put configuration file configuration options second to last. Put command line configuration
     * options at the end. Do this so they override the standard configuration.
     */
    if (g.c_config_open != NULL)
        CONFIG_APPEND(p, ",%s", g.c_config_open);
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
create_object(WT_CONNECTION *conn)
{
    WT_SESSION *session;
    size_t max;
    uint32_t maxintlkey, maxleafkey, maxleafvalue;
    char config[4096], *p;

    p = config;
    max = sizeof(config);

    CONFIG_APPEND(p,
      "key_format=%s,allocation_size=%d,%s,internal_page_max=%" PRIu32 ",leaf_page_max=%" PRIu32
      ",memory_page_max=%" PRIu32,
      (g.type == ROW) ? "u" : "r", BLOCK_ALLOCATION_SIZE,
      g.c_firstfit ? "block_allocation=first" : "", g.intl_page_max, g.leaf_page_max,
      MEGABYTE(g.c_memory_page_max));

    /*
     * Configure the maximum key/value sizes, but leave it as the default if we come up with
     * something crazy.
     */
    maxintlkey = mmrand(NULL, g.intl_page_max / 50, g.intl_page_max / 40);
    if (maxintlkey > 20)
        CONFIG_APPEND(p, ",internal_key_max=%" PRIu32, maxintlkey);
    maxleafkey = mmrand(NULL, g.leaf_page_max / 50, g.leaf_page_max / 40);
    if (maxleafkey > 20)
        CONFIG_APPEND(p, ",leaf_key_max=%" PRIu32, maxleafkey);
    maxleafvalue = mmrand(NULL, g.leaf_page_max * 10, g.leaf_page_max / 40);
    if (maxleafvalue > 40 && maxleafvalue < 100 * 1024)
        CONFIG_APPEND(p, ",leaf_value_max=%" PRIu32, maxleafvalue);

    switch (g.type) {
    case FIX:
        CONFIG_APPEND(p, ",value_format=%" PRIu32 "t", g.c_bitcnt);
        break;
    case ROW:
        CONFIG_APPEND(p, ",prefix_compression=%s,prefix_compression_min=%" PRIu32,
          g.c_prefix_compression == 0 ? "false" : "true", g.c_prefix_compression_min);
        if (g.c_reverse)
            CONFIG_APPEND(p, ",collator=reverse");
    /* FALLTHROUGH */
    case VAR:
        if (g.c_huffman_value)
            CONFIG_APPEND(p, ",huffman_value=english");
        if (g.c_dictionary)
            CONFIG_APPEND(p, ",dictionary=%" PRIu32, mmrand(NULL, 123, 517));
        break;
    }

    /* Configure checksums. */
    switch (g.c_checksum_flag) {
    case CHECKSUM_OFF:
        CONFIG_APPEND(p, ",checksum=\"off\"");
        break;
    case CHECKSUM_ON:
        CONFIG_APPEND(p, ",checksum=\"on\"");
        break;
    case CHECKSUM_UNCOMPRESSED:
        CONFIG_APPEND(p, ",checksum=\"uncompressed\"");
        break;
    case CHECKSUM_UNENCRYPTED:
        CONFIG_APPEND(p, ",checksum=\"unencrypted\"");
        break;
    }

    /* Configure compression. */
    if (g.c_compression_flag != COMPRESS_NONE)
        CONFIG_APPEND(p, ",block_compressor=\"%s\"", compressor(g.c_compression_flag));

    /* Configure Btree. */
    CONFIG_APPEND(p, ",internal_key_truncate=%s", g.c_internal_key_truncation ? "true" : "false");
    CONFIG_APPEND(p, ",split_pct=%" PRIu32, g.c_split_pct);

    /* Assertions: assertions slow down the code for additional diagnostic checking.  */
    if (g.c_assert_read_timestamp)
        CONFIG_APPEND(p, ",assert=(read_timestamp=%s)", g.c_txn_timestamps ? "always" : "never");
    if (g.c_assert_write_timestamp)
        CONFIG_APPEND(p, ",assert=(write_timestamp=on),write_timestamp_usage=%s",
          g.c_txn_timestamps ? "key_consistent" : "never");

    /* Configure LSM. */
    if (DATASOURCE("lsm")) {
        CONFIG_APPEND(p, ",type=lsm,lsm=(");
        CONFIG_APPEND(p, "auto_throttle=%s,", g.c_auto_throttle ? "true" : "false");
        CONFIG_APPEND(p, "chunk_size=%" PRIu32 "MB,", g.c_chunk_size);
        /*
         * We can't set bloom_oldest without bloom, and we want to test with Bloom filters on most
         * of the time anyway.
         */
        if (g.c_bloom_oldest)
            g.c_bloom = 1;
        CONFIG_APPEND(p, "bloom=%s,", g.c_bloom ? "true" : "false");
        CONFIG_APPEND(p, "bloom_bit_count=%" PRIu32 ",", g.c_bloom_bit_count);
        CONFIG_APPEND(p, "bloom_hash_count=%" PRIu32 ",", g.c_bloom_hash_count);
        CONFIG_APPEND(p, "bloom_oldest=%s,", g.c_bloom_oldest ? "true" : "false");
        CONFIG_APPEND(p, "merge_max=%" PRIu32 ",", g.c_merge_max);
        CONFIG_APPEND(p, ",)");
    }

    if (max == 0)
        testutil_die(ENOMEM, "WT_SESSION.create configuration buffer too small");

    /*
     * Create the underlying store.
     */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_checkfmt(session->create(session, g.uri, config), "%s", g.uri);
    testutil_check(session->close(session, NULL));
}

/*
 * wts_create --
 *     Create the database home and objects.
 */
void
wts_create(const char *home)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    size_t len;
    char *cmd;

    len = strlen(g.home) * 3 + strlen(FORMAT_HOME_INIT_CMD) + 1;
    cmd = dmalloc(len);
    testutil_check(__wt_snprintf(cmd, len, FORMAT_HOME_INIT_CMD, g.home, g.home, g.home));
    if ((ret = system(cmd)) != 0)
        testutil_die(ret, "home initialization (\"%s\") failed", cmd);
    free(cmd);

    create_database(home, &conn);
    create_object(conn);
    if (g.c_in_memory != 0)
        g.wts_conn_inmemory = conn;
    else
        testutil_check(conn->close(conn, NULL));
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

    enc = encryptor_at_open(g.c_encryption_flag);
    if (enc != NULL)
        CONFIG_APPEND(p, ",encryption=(name=%s)", enc);

    /*
     * Timing stress options aren't persisted in the base config and need to be added to the
     * configuration for re-open.
     */
    CONFIG_APPEND(p, ",timing_stress_for_test=[");
    if (g.c_timing_stress_aggressive_sweep)
        CONFIG_APPEND(p, ",aggressive_sweep");
    if (g.c_timing_stress_checkpoint)
        CONFIG_APPEND(p, ",checkpoint_slow");
    if (g.c_timing_stress_checkpoint_prepare)
        CONFIG_APPEND(p, ",prepare_checkpoint_delay");
    if (g.c_timing_stress_failpoint_hs_delete_key_from_ts)
        CONFIG_APPEND(p, ",failpoint_history_store_delete_key_from_ts");
    if (g.c_timing_stress_failpoint_hs_insert_1)
        CONFIG_APPEND(p, ",failpoint_history_store_insert_1");
    if (g.c_timing_stress_failpoint_hs_insert_2)
        CONFIG_APPEND(p, ",failpoint_history_store_insert_2");
    if (g.c_timing_stress_hs_checkpoint_delay)
        CONFIG_APPEND(p, ",history_store_checkpoint_delay");
    if (g.c_timing_stress_hs_search)
        CONFIG_APPEND(p, ",history_store_search");
    if (g.c_timing_stress_hs_sweep)
        CONFIG_APPEND(p, ",history_store_sweep_race");
    if (g.c_timing_stress_split_1)
        CONFIG_APPEND(p, ",split_1");
    if (g.c_timing_stress_split_2)
        CONFIG_APPEND(p, ",split_2");
    if (g.c_timing_stress_split_3)
        CONFIG_APPEND(p, ",split_3");
    if (g.c_timing_stress_split_4)
        CONFIG_APPEND(p, ",split_4");
    if (g.c_timing_stress_split_5)
        CONFIG_APPEND(p, ",split_5");
    if (g.c_timing_stress_split_6)
        CONFIG_APPEND(p, ",split_6");
    if (g.c_timing_stress_split_7)
        CONFIG_APPEND(p, ",split_7");
    CONFIG_APPEND(p, "]");

    /* If in-memory, there's only a single, shared WT_CONNECTION handle. */
    if (g.c_in_memory != 0)
        conn = g.wts_conn_inmemory;
    else {
#if WIREDTIGER_VERSION_MAJOR >= 10
        if (g.c_verify && allow_verify)
            CONFIG_APPEND(p, ",verify_metadata=true");
#else
        WT_UNUSED(allow_verify);
#endif
        testutil_checkfmt(wiredtiger_open(home, &event_handler, config, &conn), "%s", home);
    }

    testutil_check(conn->open_session(conn, NULL, NULL, sessionp));
    *connp = conn;
}

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

    testutil_check(conn->close(conn, g.c_leak_memory ? "leak_memory" : NULL));
}

void
wts_verify(WT_CONNECTION *conn, const char *tag)
{
    WT_DECL_RET;
    WT_SESSION *session;

    if (g.c_verify == 0)
        return;

    track("verify", 0ULL, NULL);

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    trace_msg("%s", "=============== verify start");

    /*
     * Verify can return EBUSY if the handle isn't available. Don't yield and retry, in the case of
     * LSM, the handle may not be available for a long time.
     */
    ret = session->verify(session, g.uri, "strict");
    testutil_assertfmt(ret == 0 || ret == EBUSY, "session.verify: %s: %s", g.uri, tag);

    trace_msg("%s", "=============== verify stop");
    testutil_check(session->close(session, NULL));
}

/*
 * wts_stats --
 *     Dump the run's statistics.
 */
void
wts_stats(void)
{
    FILE *fp;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    size_t len;
    uint64_t v;
    char *stat_name;
    const char *desc, *pval;

    /* Ignore statistics if they're not configured. */
    if (g.c_statistics == 0)
        return;

    conn = g.wts_conn;
    track("stat", 0ULL, NULL);

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    if ((fp = fopen(g.home_stats, "w")) == NULL)
        testutil_die(errno, "fopen: %s", g.home_stats);

    /* Connection statistics. */
    fprintf(fp, "====== Connection statistics:\n");
    testutil_check(session->open_cursor(session, "statistics:", NULL, NULL, &cursor));

    while (
      (ret = cursor->next(cursor)) == 0 && (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
        if (fprintf(fp, "%s=%s\n", desc, pval) < 0)
            testutil_die(errno, "fprintf");

    if (ret != WT_NOTFOUND)
        testutil_die(ret, "cursor.next");
    testutil_check(cursor->close(cursor));

    /* Data source statistics. */
    fprintf(fp, "\n\n====== Data source statistics:\n");
    len = strlen("statistics:") + strlen(g.uri) + 1;
    stat_name = dmalloc(len);
    testutil_check(__wt_snprintf(stat_name, len, "statistics:%s", g.uri));
    testutil_check(session->open_cursor(session, stat_name, NULL, NULL, &cursor));
    free(stat_name);

    while (
      (ret = cursor->next(cursor)) == 0 && (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
        if (fprintf(fp, "%s=%s\n", desc, pval) < 0)
            testutil_die(errno, "fprintf");

    if (ret != WT_NOTFOUND)
        testutil_die(ret, "cursor.next");
    testutil_check(cursor->close(cursor));

    fclose_and_clear(&fp);

    testutil_check(session->close(session, NULL));
}
