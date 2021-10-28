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
#include "config.h"

static void config_backup_incr(void);
static void config_backup_incr_granularity(void);
static void config_backup_incr_log_compatibility_check(void);
static void config_backward_compatible(void);
static void config_cache(void);
static void config_checkpoint(void);
static void config_checksum(void);
static void config_compression(const char *);
static void config_directio(void);
static void config_encryption(void);
static const char *config_file_type(u_int);
static bool config_fix(void);
static void config_in_memory(void);
static void config_in_memory_reset(void);
static int config_is_perm(const char *);
static void config_lsm_reset(void);
static void config_map_backup_incr(const char *, u_int *);
static void config_map_checkpoint(const char *, u_int *);
static void config_map_checksum(const char *, u_int *);
static void config_map_compression(const char *, u_int *);
static void config_map_encryption(const char *, u_int *);
static void config_map_file_type(const char *, u_int *);
static void config_pct(void);
static void config_prefix(void);
static void config_reset(void);
static void config_transaction(void);

/*
 * We currently disable random LSM testing, that is, it can be specified explicitly but we won't
 * randomly choose LSM as a data_source configuration.
 */
#define DISABLE_RANDOM_LSM_TESTING 1

/*
 * config_final --
 *     Final run initialization.
 */
void
config_final(void)
{
    config_print(false);

    g.rows = g.c_rows; /* Set the key count. */

    key_init(); /* Initialize key/value information. */
    val_init();
}

/*
 * config --
 *     Initialize the configuration itself.
 */
void
config_run(void)
{
    CONFIG *cp;
    char buf[128];

    /* Clear any temporary values. */
    config_reset();

    /* Periodically run in-memory. */
    config_in_memory();

    /*
     * Choose a file format and a data source: they're interrelated (LSM is only compatible with
     * row-store) and other items depend on them.
     */
    if (!config_is_perm("runs.type")) {
        if (config_is_perm("runs.source") && DATASOURCE("lsm"))
            config_single("runs.type=row", false);
        else
            switch (mmrand(NULL, 1, 10)) {
            case 1:
            case 2:
            case 3: /* 30% */
                config_single("runs.type=var", false);
                break;
            case 4: /* 10% */
                if (config_fix()) {
                    config_single("runs.type=fix", false);
                    break;
                }
                /* FALLTHROUGH */ /* 60% */
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
                config_single("runs.type=row", false);
                break;
            }
    }

    if (!config_is_perm("runs.source")) {
        config_single("runs.source=table", false);
        switch (mmrand(NULL, 1, 5)) {
        case 1: /* 20% */
            config_single("runs.source=file", false);
            break;
        case 2: /* 20% */
#if !defined(DISABLE_RANDOM_LSM_TESTING)
            /*
             * LSM requires a row-store and backing disk.
             *
             * Configuring truncation or timestamps results in LSM cache problems, don't configure
             * LSM if those set.
             *
             * XXX Remove the timestamp test when WT-4162 resolved.
             */
            if (g.type != ROW || g.c_in_memory)
                break;
            if (config_is_perm("transaction.timestamps") && g.c_txn_timestamps)
                break;
            if (config_is_perm("ops.truncate") && g.c_truncate)
                break;
            config_single("runs.source=lsm", false);
#endif
            break;
        case 3:
        case 4:
        case 5: /* 60% */
            break;
        }
    }

    /* If data_source and file_type were both "permanent", we may still have a mismatch. */
    if (DATASOURCE("lsm") && g.type != ROW)
        testutil_die(
          EINVAL, "%s: lsm data_source is only compatible with row file_type\n", progname);

    /*
     * Build the top-level object name: we're overloading data_source in our configuration, LSM
     * objects are "tables", but files are tested as well.
     */
    g.uri = dmalloc(256);
    strcpy(g.uri, DATASOURCE("file") ? "file:" : "table:");
    strcat(g.uri, WT_NAME);

    /* Fill in random values for the rest of the run. */
    for (cp = c; cp->name != NULL; ++cp) {
        if (F_ISSET(cp, C_IGNORE | C_PERM | C_TEMP))
            continue;

        /*
         * Boolean flags are 0 or 1, where the variable's "min" value is the percent chance the flag
         * is "on" (so "on" if random rolled <= N, otherwise "off").
         */
        if (F_ISSET(cp, C_BOOL))
            testutil_check(__wt_snprintf(
              buf, sizeof(buf), "%s=%s", cp->name, mmrand(NULL, 1, 100) <= cp->min ? "on" : "off"));
        else
            testutil_check(__wt_snprintf(
              buf, sizeof(buf), "%s=%" PRIu32, cp->name, mmrand(NULL, cp->min, cp->maxrand)));
        config_single(buf, false);
    }

    /* Only row-store tables support collation order. */
    if (g.type != ROW)
        config_single("btree.reverse=off", false);

    /* First, transaction configuration, it configures other features. */
    config_transaction();

    /* Simple selection. */
    config_backup_incr();
    config_checkpoint();
    config_checksum();
    config_compression("btree.compression");
    config_compression("logging.compression");
    config_encryption();
    config_prefix();

    /* Configuration based on the configuration already chosen. */
    config_directio();
    config_pct();
    config_cache();

    /* Give in-memory, LSM and backward compatible configurations a final review. */
    if (g.c_in_memory != 0)
        config_in_memory_reset();
    if (DATASOURCE("lsm"))
        config_lsm_reset();
    config_backward_compatible();

    /*
     * Key/value minimum/maximum are related, correct unless specified by the configuration.
     */
    if (!config_is_perm("btree.key_min") && g.c_key_min > g.c_key_max)
        g.c_key_min = g.c_key_max;
    if (!config_is_perm("btree.key_max") && g.c_key_max < g.c_key_min)
        g.c_key_max = g.c_key_min;
    if (g.c_key_min > g.c_key_max)
        testutil_die(EINVAL, "key_min may not be larger than key_max");

    if (!config_is_perm("btree.value_min") && g.c_value_min > g.c_value_max)
        g.c_value_min = g.c_value_max;
    if (!config_is_perm("btree.value_max") && g.c_value_max < g.c_value_min)
        g.c_value_max = g.c_value_min;
    if (g.c_value_min > g.c_value_max)
        testutil_die(EINVAL, "value_min may not be larger than value_max");

    /*
     * Run-length is configured by a number of operations and a timer.
     *
     * If the operation count and the timer are both configured, do nothing. If only the timer is
     * configured, clear the operations count. If only the operation count is configured, limit the
     * run to 6 hours. If neither is configured, leave the operations count alone and limit the run
     * to 30 minutes.
     *
     * In other words, if we rolled the dice on everything, do a short run. If we chose a number of
     * operations but the rest of the configuration means operations take a long time to complete
     * (for example, a small cache and many worker threads), don't let it run forever.
     */
    if (config_is_perm("runs.timer")) {
        if (!config_is_perm("runs.ops"))
            config_single("runs.ops=0", false);
    } else {
        if (!config_is_perm("runs.ops"))
            config_single("runs.timer=30", false);
        else
            config_single("runs.timer=360", false);
    }
}

/*
 * config_backup_incr --
 *     Incremental backup configuration.
 */
static void
config_backup_incr(void)
{
    /* Incremental backup requires backup. */
    if (g.c_backups == 0) {
        if (!config_is_perm("backup.incremental"))
            config_single("backup.incremental=off", false);
        if (g.c_backup_incr_flag != INCREMENTAL_OFF)
            testutil_die(EINVAL, "backup.incremental requires backups be configured");
        return;
    }

    /*
     * Incremental backup using log files is incompatible with logging archival. Testing log file
     * archival doesn't seem as useful as testing backup, let the backup configuration override.
     */
    if (config_is_perm("backup.incremental")) {
        if (g.c_backup_incr_flag == INCREMENTAL_LOG)
            config_backup_incr_log_compatibility_check();
        if (g.c_backup_incr_flag == INCREMENTAL_BLOCK)
            config_backup_incr_granularity();
        return;
    }

    /*
     * Choose a type of incremental backup, where the log archival setting can eliminate incremental
     * backup based on log files.
     */
    switch (mmrand(NULL, 1, 10)) {
    case 1: /* 30% full backup only */
    case 2:
    case 3:
        config_single("backup.incremental=off", false);
        break;
    case 4: /* 30% log based incremental */
    case 5:
    case 6:
        if (!g.c_logging_archive || !config_is_perm("logging.archive")) {
            if (g.c_logging_archive)
                config_single("logging.archive=0", false);
            config_single("backup.incremental=log", false);
            break;
        }
    /* FALLTHROUGH */
    case 7: /* 40% block based incremental */
    case 8:
    case 9:
    case 10:
        config_single("backup.incremental=block", false);
        config_backup_incr_granularity();
        break;
    }
}

/*
 * config_backup_incr_granularity --
 *     Configuration of block granularity for incremental backup
 */
static void
config_backup_incr_granularity(void)
{
    uint32_t granularity, i;
    char confbuf[128];

    if (config_is_perm("backup.incr_granularity"))
        return;

    /*
     * Three block sizes are interesting. 16MB is the default for WiredTiger and MongoDB. 1MB is the
     * minimum allowed by MongoDB. Smaller sizes stress block tracking and are good for testing. The
     * granularity is in units of KB.
     */
    granularity = 0;
    i = mmrand(NULL, 1, 10);
    switch (i) {
    case 1: /* 50% small size for stress testing */
    case 2:
    case 3:
    case 4:
    case 5:
        granularity = 4 * i;
        break;
    case 6: /* 20% 1MB granularity */
    case 7:
        granularity = 1024;
        break;
    case 8: /* 30% 16MB granularity */
    case 9:
    case 10:
        granularity = 16 * 1024;
        break;
    }

    testutil_check(
      __wt_snprintf(confbuf, sizeof(confbuf), "backup.incr_granularity=%u", granularity));
    config_single(confbuf, false);
}

/*
 * config_backward_compatible --
 *     Backward compatibility configuration.
 */
static void
config_backward_compatible(void)
{
    bool backward_compatible;

    /*
     * If built in a branch that doesn't support all current options, or creating a database for
     * such an environment, strip out configurations that won't work.
     */
    backward_compatible = g.backward_compatible;
#if WIREDTIGER_VERSION_MAJOR < 10
    backward_compatible = true;
#endif
    if (!backward_compatible)
        return;

    if (g.c_mmap_all) {
        if (config_is_perm("disk.mmap_all"))
            testutil_die(EINVAL, "disk.mmap_all not supported in backward compatibility mode");
        config_single("disk.mmap_all=off", false);
    }

    if (g.c_timing_stress_checkpoint_reserved_txnid_delay) {
        if (config_is_perm("stress.checkpoint_reserved_txnid_delay"))
            testutil_die(EINVAL,
              "stress.checkpoint_reserved_txnid_delay not supported in backward compatibility "
              "mode");
        config_single("stress.checkpoint_reserved_txnid_delay=off", false);
    }

    if (g.c_timing_stress_hs_sweep) {
        if (config_is_perm("stress.hs_sweep"))
            testutil_die(EINVAL, "stress.hs_sweep not supported in backward compatibility mode");
        config_single("stress.hs_sweep=off", false);
    }

    if (g.c_timing_stress_hs_checkpoint_delay) {
        if (config_is_perm("stress.hs_checkpoint_delay"))
            testutil_die(
              EINVAL, "stress.hs_checkpoint_delay not supported in backward compatibility mode");
        config_single("stress.hs_checkpoint_delay=off", false);
    }

    if (g.c_timing_stress_hs_search) {
        if (config_is_perm("stress.hs_search"))
            testutil_die(EINVAL, "stress.hs_search not supported in backward compatibility mode");
        config_single("stress.hs_search=off", false);
    }
}

/*
 * config_cache --
 *     Cache configuration.
 */
static void
config_cache(void)
{
    uint32_t required, workers;

    /* Page sizes are powers-of-two for bad historic reasons. */
    g.intl_page_max = 1U << g.c_intl_page_max;
    g.leaf_page_max = 1U << g.c_leaf_page_max;

    /* Check if a minimum cache size has been specified. */
    if (config_is_perm("cache")) {
        if (config_is_perm("cache.minimum") && g.c_cache_minimum != 0 &&
          g.c_cache < g.c_cache_minimum)
            testutil_die(EINVAL, "minimum cache set larger than cache (%" PRIu32 " > %" PRIu32 ")",
              g.c_cache_minimum, g.c_cache);
        return;
    }

    g.c_cache = WT_MAX(g.c_cache, g.c_cache_minimum);

    /*
     * Maximum internal/leaf page size sanity.
     *
     * Ensure we can service at least one operation per-thread concurrently without filling the
     * cache with pinned pages, that is, every thread consuming an internal page and a leaf page (or
     * a pair of leaf pages for cursor movements).
     *
     * Maximum memory pages are in units of MB.
     *
     * This code is what dramatically increases the cache size when there are lots of threads, it
     * grows the cache to several megabytes per thread.
     */
    workers = g.c_threads;
    if (g.c_hs_cursor)
        ++workers;
    if (g.c_random_cursor)
        ++workers;
    g.c_cache = WT_MAX(g.c_cache, 2 * workers * g.c_memory_page_max);

    /*
     * Ensure cache size sanity for LSM runs. An LSM tree open requires 3 chunks plus a page for
     * each participant in up to three concurrent merges. Integrate a thread count into that
     * calculation by requiring 3 chunks/pages per configured thread. That might be overkill, but
     * LSM runs are more sensitive to small caches than other runs, and a generous cache avoids
     * stalls we're not interested in chasing.
     */
    if (DATASOURCE("lsm")) {
        required = WT_LSM_TREE_MINIMUM_SIZE(
          g.c_chunk_size * WT_MEGABYTE, workers * g.c_merge_max, workers * g.leaf_page_max);
        required = (required + (WT_MEGABYTE - 1)) / WT_MEGABYTE;
        if (g.c_cache < required)
            g.c_cache = required;
    }
}

/*
 * config_checkpoint --
 *     Checkpoint configuration.
 */
static void
config_checkpoint(void)
{
    /* Choose a checkpoint mode if nothing was specified. */
    if (!config_is_perm("checkpoint"))
        switch (mmrand(NULL, 1, 20)) {
        case 1:
        case 2:
        case 3:
        case 4: /* 20% */
            config_single("checkpoint=wiredtiger", false);
            break;
        case 5: /* 5 % */
            config_single("checkpoint=off", false);
            break;
        default: /* 75% */
            config_single("checkpoint=on", false);
            break;
        }
}

/*
 * config_checksum --
 *     Checksum configuration.
 */
static void
config_checksum(void)
{
    /* Choose a checksum mode if nothing was specified. */
    if (!config_is_perm("disk.checksum"))
        switch (mmrand(NULL, 1, 10)) {
        case 1:
        case 2:
        case 3:
        case 4: /* 40% */
            config_single("disk.checksum=on", false);
            break;
        case 5: /* 10% */
            config_single("disk.checksum=off", false);
            break;
        case 6: /* 10% */
            config_single("disk.checksum=uncompressed", false);
            break;
        default: /* 40% */
            config_single("disk.checksum=unencrypted", false);
            break;
        }
}

/*
 * config_compression --
 *     Compression configuration.
 */
static void
config_compression(const char *conf_name)
{
    char confbuf[128];
    const char *cstr;

    /* Return if already specified. */
    if (config_is_perm(conf_name))
        return;

    /*
     * Don't configure a compression engine for logging if logging isn't configured (it won't break,
     * but it's confusing).
     */
    cstr = "none";
    if (strcmp(conf_name, "logging.compression") == 0 && g.c_logging == 0) {
        testutil_check(__wt_snprintf(confbuf, sizeof(confbuf), "%s=%s", conf_name, cstr));
        config_single(confbuf, false);
        return;
    }

    /*
     * Select a compression type from the list of built-in engines.
     *
     * Listed percentages are only correct if all of the possible engines are compiled in.
     */
    switch (mmrand(NULL, 1, 20)) {
#ifdef HAVE_BUILTIN_EXTENSION_LZ4
    case 1:
    case 2:
    case 3: /* 15% lz4 */
        cstr = "lz4";
        break;
#endif
#ifdef HAVE_BUILTIN_EXTENSION_SNAPPY
    case 4:
    case 5:
    case 6:
    case 7: /* 30% snappy */
    case 8:
    case 9:
        cstr = "snappy";
        break;
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZLIB
    case 10:
    case 11:
    case 12:
    case 13: /* 20% zlib */
        cstr = "zlib";
        break;
#endif
#ifdef HAVE_BUILTIN_EXTENSION_ZSTD
    case 14:
    case 15:
    case 16:
    case 17: /* 20% zstd */
        cstr = "zstd";
        break;
#endif
    case 18:
    case 19:
    case 20: /* 15% no compression */
    default:
        break;
    }

    testutil_check(__wt_snprintf(confbuf, sizeof(confbuf), "%s=%s", conf_name, cstr));
    config_single(confbuf, false);
}

/*
 * config_directio
 *     Direct I/O configuration.
 */
static void
config_directio(void)
{
    /*
     * We don't roll the dice and set direct I/O, it has to be set explicitly. For that reason, any
     * incompatible "permanent" option set with direct I/O is a configuration error.
     */
    if (!g.c_direct_io)
        return;

    /*
     * Direct I/O may not work with backups, doing copies through the buffer cache after configuring
     * direct I/O in Linux won't work. If direct I/O is configured, turn off backups.
     */
    if (g.c_backups) {
        if (config_is_perm("backup"))
            testutil_die(EINVAL, "direct I/O is incompatible with backup configurations");
        config_single("backup=off", false);
    }

    /*
     * Direct I/O may not work with imports for the same reason as for backups.
     */
    if (g.c_import) {
        if (config_is_perm("import"))
            testutil_die(EINVAL, "direct I/O is incompatible with import configurations");
        config_single("import=0", false);
    }

    /*
     * Direct I/O may not work with mmap. Theoretically, Linux ignores direct I/O configurations in
     * the presence of shared cache configurations (including mmap), but we've seen file corruption
     * and it doesn't make much sense (the library disallows the combination).
     */
    if (g.c_mmap_all != 0) {
        if (config_is_perm("disk.mmap_all"))
            testutil_die(EINVAL, "direct I/O is incompatible with mmap_all configurations");
        config_single("disk.mmap_all=off", false);
    }

    /*
     * Turn off all external programs. Direct I/O is really, really slow on some machines and it can
     * take hours for a job to run. External programs don't have timers running so it looks like
     * format just hung, and the 15-minute timeout isn't effective. We could play games to handle
     * child process termination, but it's not worth the effort.
     */
    if (g.c_salvage) {
        if (config_is_perm("ops.salvage"))
            testutil_die(EINVAL, "direct I/O is incompatible with salvage configurations");
        config_single("ops.salvage=off", false);
    }
}

/*
 * config_encryption --
 *     Encryption configuration.
 */
static void
config_encryption(void)
{
    const char *cstr;

    /*
     * Encryption: choose something if encryption wasn't specified.
     */
    if (!config_is_perm("disk.encryption")) {
        cstr = "disk.encryption=none";
        switch (mmrand(NULL, 1, 10)) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5: /* 70% no encryption */
        case 6:
        case 7:
            break;
        case 8:
        case 9:
        case 10: /* 30% rotn */
            cstr = "disk.encryption=rotn-7";
            break;
        }

        config_single(cstr, false);
    }
}

/*
 * config_fix --
 *     Fixed-length column-store configuration.
 */
static bool
config_fix(void)
{
    /* Fixed-length column stores don't support the history store table, so no modify operations. */
    if (config_is_perm("ops.pct.modify"))
        return (false);
    return (true);
}

/*
 * config_in_memory --
 *     Periodically set up an in-memory configuration.
 */
static void
config_in_memory(void)
{
    /*
     * Configure in-memory before configuring anything else, in-memory has many related
     * requirements. Don't configure in-memory if there's any incompatible configurations, so we
     * don't have to configure in-memory every time we configure something like LSM, that's too
     * painful.
     */
    if (config_is_perm("backup"))
        return;
    if (config_is_perm("btree.compression"))
        return;
    if (config_is_perm("checkpoint"))
        return;
    if (config_is_perm("format.abort"))
        return;
    if (config_is_perm("import"))
        return;
    if (config_is_perm("logging"))
        return;
    if (config_is_perm("ops.hs_cursor"))
        return;
    if (config_is_perm("ops.salvage"))
        return;
    if (config_is_perm("ops.verify"))
        return;
    if (config_is_perm("runs.source") && DATASOURCE("lsm"))
        return;

    if (!config_is_perm("runs.in_memory") && mmrand(NULL, 1, 20) == 1)
        g.c_in_memory = 1;
}

/*
 * config_in_memory_reset --
 *     In-memory configuration review.
 */
static void
config_in_memory_reset(void)
{
    uint32_t cache;

    /* Turn off a lot of stuff. */
    if (!config_is_perm("backup"))
        config_single("backup=off", false);
    if (!config_is_perm("btree.compression"))
        config_single("btree.compression=none", false);
    if (!config_is_perm("checkpoint"))
        config_single("checkpoint=off", false);
    if (!config_is_perm("import"))
        config_single("import=off", false);
    if (!config_is_perm("logging"))
        config_single("logging=off", false);
    if (!config_is_perm("ops.alter"))
        config_single("ops.alter=off", false);
    if (!config_is_perm("ops.hs_cursor"))
        config_single("ops.hs_cursor=off", false);
    if (!config_is_perm("ops.salvage"))
        config_single("ops.salvage=off", false);
    if (!config_is_perm("ops.verify"))
        config_single("ops.verify=off", false);

    /*
     * Keep keys/values small, overflow items aren't an issue for in-memory configurations and it
     * keeps us from overflowing the cache.
     */
    if (!config_is_perm("btree.key_max"))
        config_single("btree.key_max=32", false);
    if (!config_is_perm("btree.value_max"))
        config_single("btree.value_max=80", false);

    /*
     * Size the cache relative to the initial data set, use 2x the base size as a minimum.
     */
    if (!config_is_perm("cache")) {
        cache = g.c_value_max;
        if (g.type == ROW)
            cache += g.c_key_max;
        cache *= g.c_rows;
        cache *= 2;
        cache /= WT_MEGABYTE;
        if (g.c_cache < cache)
            g.c_cache = cache;
    }
}

/*
 * config_backup_incr_compatibility_check --
 *     Backup incremental log compatibility check.
 */
static void
config_backup_incr_log_compatibility_check(void)
{
    /*
     * Incremental backup using log files is incompatible with logging archival. Disable logging
     * archival if log incremental backup is set.
     */
    if (g.c_logging_archive && config_is_perm("logging.archive"))
        testutil_die(EINVAL, "backup.incremental=log is incompatible with logging.archive");
    if (g.c_logging_archive)
        config_single("logging.archive=0", false);
}

/*
 * config_lsm_reset --
 *     LSM configuration review.
 */
static void
config_lsm_reset(void)
{
    /*
     * Turn off truncate for LSM runs (some configurations with truncate always result in a
     * timeout).
     */
    if (!config_is_perm("ops.truncate"))
        config_single("ops.truncate=off", false);

    /*
     * LSM doesn't currently play nicely with timestamps, don't choose the pair unless forced to. If
     * we turn off timestamps, make sure we turn off prepare as well, it requires timestamps. Remove
     * this code with WT-4162.
     */
    if (!config_is_perm("ops.prepare") && !config_is_perm("transaction.timestamps")) {
        config_single("ops.prepare=off", false);
        config_single("transaction.timestamps=off", false);
    }

    /*
     * LSM does not work with block-based incremental backup, change the incremental backup
     * mechanism if block based in configured.
     */
    if (g.c_backups) {
        if (config_is_perm("backup.incremental") && g.c_backup_incr_flag == INCREMENTAL_BLOCK)
            testutil_die(EINVAL, "LSM does not work with backup.incremental=block configuration.");

        if (g.c_backup_incr_flag == INCREMENTAL_BLOCK)
            switch (mmrand(NULL, 1, 2)) {
            case 1:
                /* 50% */
                config_single("backup.incremental=off", false);
                break;
            case 2:
                /* 50% */
                config_single("backup.incremental=log", false);
                config_backup_incr_log_compatibility_check();
                break;
            }
    }
}

/*
 * config_pct --
 *     Configure operation percentages.
 */
static void
config_pct(void)
{
    static struct {
        const char *name; /* Operation */
        uint32_t *vp;     /* Value store */
        u_int order;      /* Order of assignment */
    } list[] = {
      {"ops.pct.delete", &g.c_delete_pct, 0},
      {"ops.pct.insert", &g.c_insert_pct, 0},
#define CONFIG_MODIFY_ENTRY 2
      {"ops.pct.modify", &g.c_modify_pct, 0},
      {"ops.pct.read", &g.c_read_pct, 0},
      {"ops.pct.write", &g.c_write_pct, 0},
    };
    u_int i, max_order, max_slot, n, pct;

    /*
     * Walk the list of operations, checking for an illegal configuration and creating a random
     * order in the list.
     */
    pct = 0;
    for (i = 0; i < WT_ELEMENTS(list); ++i)
        if (config_is_perm(list[i].name))
            pct += *list[i].vp;
        else
            list[i].order = mmrand(NULL, 1, 1000);
    if (pct > 100)
        testutil_die(EINVAL, "operation percentages do not total to 100%%");

    /* Cursor modify isn't possible for fixed-length column store. */
    if (g.type == FIX) {
        if (config_is_perm("ops.pct.modify") && g.c_modify_pct != 0)
            testutil_die(EINVAL, "WT_CURSOR.modify not supported by fixed-length column store");
        list[CONFIG_MODIFY_ENTRY].order = 0;
        *list[CONFIG_MODIFY_ENTRY].vp = 0;
    }

    /*
     * Walk the list, allocating random numbers of operations in a random order.
     *
     * If the "order" field is non-zero, we need to create a value for this operation. Find the
     * largest order field in the array; if one non-zero order field is found, it's the last entry
     * and gets the remainder of the operations.
     */
    for (pct = 100 - pct;;) {
        for (i = n = max_order = max_slot = 0; i < WT_ELEMENTS(list); ++i) {
            if (list[i].order != 0)
                ++n;
            if (list[i].order > max_order) {
                max_order = list[i].order;
                max_slot = i;
            }
        }
        if (n == 0)
            break;
        if (n == 1) {
            *list[max_slot].vp = pct;
            break;
        }
        *list[max_slot].vp = mmrand(NULL, 0, pct);
        list[max_slot].order = 0;
        pct -= *list[max_slot].vp;
    }

    testutil_assert(
      g.c_delete_pct + g.c_insert_pct + g.c_modify_pct + g.c_read_pct + g.c_write_pct == 100);
}

/*
 * config_prefix --
 *     Prefix configuration.
 */
static void
config_prefix(void)
{
    /* Add prefix compression if prefixes are configured and no explicit choice was made. */
    if (g.c_prefix != 0 && g.c_prefix_compression == 0 &&
      !config_is_perm("btree.prefix_compression"))
        config_single("btree.prefix_compression=on", false);
}

/*
 * config_transaction --
 *     Transaction configuration.
 */
static void
config_transaction(void)
{
    /* Transaction prepare requires timestamps and is incompatible with logging. */
    if (g.c_prepare && config_is_perm("ops.prepare")) {
        if (g.c_logging && config_is_perm("logging"))
            testutil_die(EINVAL, "prepare is incompatible with logging");
        if (!g.c_txn_timestamps && config_is_perm("transaction.timestamps"))
            testutil_die(EINVAL, "prepare requires transaction timestamps");
    }

    /* Transaction timestamps are incompatible with implicit transactions. */
    if (g.c_txn_timestamps && config_is_perm("transaction.timestamps")) {
        if (g.c_txn_implicit && config_is_perm("transaction.implicit"))
            testutil_die(
              EINVAL, "transaction.timestamps is incompatible with implicit transactions");

        /* FIXME-WT-6431: temporarily disable salvage with timestamps. */
        if (g.c_salvage && config_is_perm("ops.salvage"))
            testutil_die(EINVAL, "transaction.timestamps is incompatible with salvage");
    }

    /*
     * Incompatible permanent configurations have been checked, now turn off any incompatible flags.
     * The choices are inclined to prepare (it's only rarely configured), then timestamps. Note any
     * of the options may still be set as required for the run, so we still have to check if that's
     * the case until we run out of combinations (for example, prepare turns off logging, so by the
     * time we check logging, logging must have been required by the run if both logging and prepare
     * are still set, so we can just turn off prepare in that case).
     */
    if (g.c_prepare) {
        if (!config_is_perm("logging"))
            config_single("logging=off", false);
        if (!config_is_perm("transaction.timestamps"))
            config_single("transaction.timestamps=on", false);
    }
    if (g.c_txn_timestamps) {
        if (!config_is_perm("transaction.implicit"))
            config_single("transaction.implicit=0", false);
        if (!config_is_perm("ops.salvage"))
            config_single("ops.salvage=off", false);
    }
    if (g.c_logging)
        config_single("ops.prepare=off", false);
    if (g.c_txn_implicit)
        config_single("transaction.timestamps=off", false);
    if (g.c_salvage)
        config_single("transaction.timestamps=off", false);
}

/*
 * config_error --
 *     Display configuration information on error.
 */
void
config_error(void)
{
    CONFIG *cp;
    size_t max_name;

    /* Display configuration names. */
    fprintf(stderr, "\n");
    fprintf(stderr, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
    fprintf(stderr, "Configuration values:\n");
    fprintf(stderr, "%10s: %s\n", "off", "boolean off");
    fprintf(stderr, "%10s: %s\n", "on", "boolean on");
    fprintf(stderr, "%10s: %s\n", "0", "boolean off");
    fprintf(stderr, "%10s: %s\n", "1", "boolean on");
    fprintf(stderr, "%10s: %s\n", "NNN", "unsigned number");
    fprintf(stderr, "%10s: %s\n", "NNN-NNN", "number range, each number equally likely");
    fprintf(stderr, "%10s: %s\n", "NNN:NNN", "number range, lower numbers more likely");
    fprintf(stderr, "%10s: %s\n", "string", "configuration value");
    fprintf(stderr, "\n");
    fprintf(stderr, "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n");
    fprintf(stderr, "Configuration names:\n");
    for (max_name = 0, cp = c; cp->name != NULL; ++cp)
        max_name = WT_MAX(max_name, strlen(cp->name));
    for (cp = c; cp->name != NULL; ++cp)
        fprintf(stderr, "%*s: %s\n", (int)max_name, cp->name, cp->desc);
}

/*
 * config_print --
 *     Print configuration information.
 */
void
config_print(bool error_display)
{
    CONFIG *cp;
    FILE *fp;

    /* Reopening an existing database should leave the existing CONFIG file. */
    if (g.reopen)
        return;

    if (error_display)
        fp = stdout;
    else if ((fp = fopen(g.home_config, "w")) == NULL)
        testutil_die(errno, "fopen: %s", g.home_config);

    fprintf(fp, "############################################\n");
    fprintf(fp, "#  RUN PARAMETERS: V2\n");
    fprintf(fp, "############################################\n");

    /* Display configuration values. */
    for (cp = c; cp->name != NULL; ++cp)
        if (F_ISSET(cp, C_STRING))
            fprintf(fp, "%s=%s\n", cp->name, *cp->vstr == NULL ? "" : *cp->vstr);
        else
            fprintf(fp, "%s=%" PRIu32 "\n", cp->name, *cp->v);

    fprintf(fp, "############################################\n");

    /* Flush so we're up-to-date on error. */
    (void)fflush(fp);

    if (fp != stdout)
        fclose_and_clear(&fp);
}

/*
 * config_file --
 *     Read configuration values from a file.
 */
void
config_file(const char *name)
{
    FILE *fp;
    char buf[256], *p, *t;

    if ((fp = fopen(name, "r")) == NULL)
        testutil_die(errno, "fopen: %s", name);

    /*
     * Skip leading Evergreen timestamps by skipping up to a closing brace and following whitespace.
     * This is a little fragile: we're in trouble if Evergreen changes its timestamp format or if
     * this program includes closing braces in its commands.
     */
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        for (p = t = buf; *p != '\0'; ++p) {
            if (*p == '\n') { /* Configuration end. */
                *p = '\0';
                break;
            }
            if (*p == '#') { /* Comment, skip the line */
                t = p;
                break;
            }
            if (t == buf && *p == ']') { /* Closing brace, configuration starts after it. */
                while (isblank((unsigned char)*++p))
                    ;
                t = p--;
            }
        }
        if (*t == '\0' || *t == '#')
            continue;
        config_single(t, true);
    }
    fclose_and_clear(&fp);
}

/*
 * config_clear --
 *     Clear all configuration values.
 */
void
config_clear(void)
{
    CONFIG *cp;

    /* Clear all allocated configuration data. */
    for (cp = c; cp->name != NULL; ++cp)
        if (cp->vstr != NULL) {
            free((void *)*cp->vstr);
            *cp->vstr = NULL;
        }
    free(g.uri);
    g.uri = NULL;
}

/*
 * config_reset --
 *     Clear per-run configuration values.
 */
static void
config_reset(void)
{
    CONFIG *cp;

    /* Clear temporary allocated configuration data. */
    for (cp = c; cp->name != NULL; ++cp) {
        F_CLR(cp, C_TEMP);
        if (!F_ISSET(cp, C_PERM) && cp->vstr != NULL) {
            free((void *)*cp->vstr);
            *cp->vstr = NULL;
        }
    }
    free(g.uri);
    g.uri = NULL;
}

/*
 * config_find
 *	Find a specific configuration entry.
 */
static CONFIG *
config_find(const char *s, size_t len, bool fatal)
{
    CONFIG *cp;

    for (cp = c; cp->name != NULL; ++cp)
        if (strncmp(s, cp->name, len) == 0 && cp->name[len] == '\0')
            return (cp);

    /* Optionally ignore unknown keywords, it makes it easier to run old CONFIG files. */
    if (fatal)
        testutil_die(EINVAL, "%s: %s: unknown required configuration keyword\n", progname, s);

    fprintf(stderr, "%s: %s: WARNING, ignoring unknown configuration keyword\n", progname, s);
    return (NULL);
}

/*
 * config_value --
 *     String to long helper function.
 */
static uint32_t
config_value(const char *config, const char *p, int match)
{
    long v;
    char *endptr;

    errno = 0;
    v = strtol(p, &endptr, 10);
    if ((errno == ERANGE && (v == LONG_MAX || v == LONG_MIN)) || (errno != 0 && v == 0) ||
      *endptr != match || v < 0 || v > UINT32_MAX)
        testutil_die(
          EINVAL, "%s: %s: illegal numeric value or value out of range", progname, config);
    return ((uint32_t)v);
}

/*
 * config_single --
 *     Set a single configuration structure value.
 */
void
config_single(const char *s, bool perm)
{
    enum { RANGE_FIXED, RANGE_NONE, RANGE_WEIGHTED } range;
    CONFIG *cp;
    uint32_t steps, v1, v2;
    u_int i;
    const char *equalp, *vp1, *vp2;

    while (__wt_isspace((u_char)*s))
        ++s;

    config_compat(&s);

    if ((equalp = strchr(s, '=')) == NULL)
        testutil_die(EINVAL, "%s: %s: illegal configuration value\n", progname, s);

    if ((cp = config_find(s, (size_t)(equalp - s), false)) == NULL)
        return;

    F_SET(cp, perm ? C_PERM : C_TEMP);
    ++equalp;

    if (F_ISSET(cp, C_STRING)) {
        /*
         * Free the previous setting if a configuration has been passed in twice.
         */
        if (*cp->vstr != NULL) {
            free(*cp->vstr);
            *cp->vstr = NULL;
        }

        if (strncmp(s, "backup.incremental", strlen("backup.incremental")) == 0) {
            config_map_backup_incr(equalp, &g.c_backup_incr_flag);
            *cp->vstr = dstrdup(equalp);
        } else if (strncmp(s, "checkpoint", strlen("checkpoint")) == 0) {
            config_map_checkpoint(equalp, &g.c_checkpoint_flag);
            *cp->vstr = dstrdup(equalp);
        } else if (strncmp(s, "disk.checksum", strlen("disk.checksum")) == 0) {
            config_map_checksum(equalp, &g.c_checksum_flag);
            *cp->vstr = dstrdup(equalp);
        } else if (strncmp(s, "btree.compression", strlen("btree.compression")) == 0) {
            config_map_compression(equalp, &g.c_compression_flag);
            *cp->vstr = dstrdup(equalp);
        } else if (strncmp(s, "runs.source", strlen("runs.source")) == 0 &&
          strncmp("file", equalp, strlen("file")) != 0 &&
          strncmp("lsm", equalp, strlen("lsm")) != 0 &&
          strncmp("table", equalp, strlen("table")) != 0) {
            testutil_die(EINVAL, "Invalid data source option: %s\n", equalp);
        } else if (strncmp(s, "disk.encryption", strlen("disk.encryption")) == 0) {
            config_map_encryption(equalp, &g.c_encryption_flag);
            *cp->vstr = dstrdup(equalp);
        } else if (strncmp(s, "runs.type", strlen("runs.type")) == 0) {
            config_map_file_type(equalp, &g.type);
            *cp->vstr = dstrdup(config_file_type(g.type));
        } else if (strncmp(s, "logging.compression", strlen("logging.compression")) == 0) {
            config_map_compression(equalp, &g.c_logging_compression_flag);
            *cp->vstr = dstrdup(equalp);
        } else
            *cp->vstr = dstrdup(equalp);

        return;
    }

    if (F_ISSET(cp, C_BOOL)) {
        if (strncmp(equalp, "off", strlen("off")) == 0)
            v1 = 0;
        else if (strncmp(equalp, "on", strlen("on")) == 0)
            v1 = 1;
        else {
            v1 = config_value(s, equalp, '\0');
            if (v1 != 0 && v1 != 1)
                testutil_die(EINVAL, "%s: %s: value of boolean not 0 or 1", progname, s);
        }

        *cp->v = v1;
        return;
    }

    /*
     * Three possible syntax elements: a number, two numbers separated by a dash, two numbers
     * separated by an colon. The first is a fixed value, the second is a range where all values are
     * equally possible, the third is a weighted range where lower values are more likely.
     */
    vp1 = equalp;
    range = RANGE_NONE;
    if ((vp2 = strchr(vp1, '-')) != NULL) {
        ++vp2;
        range = RANGE_FIXED;
    } else if ((vp2 = strchr(vp1, ':')) != NULL) {
        ++vp2;
        range = RANGE_WEIGHTED;
    }

    v1 = config_value(s, vp1, range == RANGE_NONE ? '\0' : (range == RANGE_FIXED ? '-' : ':'));
    if (v1 < cp->min || v1 > cp->maxset)
        testutil_die(EINVAL, "%s: %s: value outside min/max values of %" PRIu32 "-%" PRIu32 "\n",
          progname, s, cp->min, cp->maxset);

    if (range != RANGE_NONE) {
        v2 = config_value(s, vp2, '\0');
        if (v2 < cp->min || v2 > cp->maxset)
            testutil_die(EINVAL,
              "%s: %s: value outside min/max values of %" PRIu32 "-%" PRIu32 "\n", progname, s,
              cp->min, cp->maxset);
        if (v1 > v2)
            testutil_die(EINVAL, "%s: %s: illegal numeric range\n", progname, s);

        if (range == RANGE_FIXED)
            v1 = mmrand(NULL, (u_int)v1, (u_int)v2);
        else {
            /*
             * Roll dice, 50% chance of proceeding to the next larger value, and 5 steps to the
             * maximum value.
             */
            steps = ((v2 - v1) + 4) / 5;
            if (steps == 0)
                steps = 1;
            for (i = 0; i < 5; ++i, v1 += steps)
                if (mmrand(NULL, 0, 1) == 0)
                    break;
            v1 = WT_MIN(v1, v2);
        }
    }

    *cp->v = v1;
}

/*
 * config_map_file_type --
 *     Map a file type configuration to a flag.
 */
static void
config_map_file_type(const char *s, u_int *vp)
{
    uint32_t v;
    const char *arg;
    bool fix, row, var;

    arg = s;

    /* Accumulate choices. */
    fix = row = var = false;
    while (*s != '\0') {
        if (WT_PREFIX_SKIP(s, "fixed-length column-store") || WT_PREFIX_SKIP(s, "fix"))
            fix = true;
        else if (WT_PREFIX_SKIP(s, "row-store") || WT_PREFIX_SKIP(s, "row"))
            row = true;
        else if (WT_PREFIX_SKIP(s, "variable-length column-store") || WT_PREFIX_SKIP(s, "var"))
            var = true;
        else
            testutil_die(EINVAL, "illegal file type configuration: %s", arg);

        if (*s == ',') /* Allow, but don't require, comma-separators. */
            ++s;
    }
    if (!fix && !row && !var)
        testutil_die(EINVAL, "illegal file type configuration: %s", arg);

    /* Check for a single configuration. */
    if (fix && !row && !var) {
        *vp = FIX;
        return;
    }
    if (!fix && row && !var) {
        *vp = ROW;
        return;
    }
    if (!fix && !row && var) {
        *vp = VAR;
        return;
    }

    /*
     * Handle multiple configurations.
     *
     * Fixed-length column-store is 10% in all cases.
     *
     * Variable-length column-store is 90% vs. fixed, 30% vs. fixed and row, and 40% vs row.
     */
    v = mmrand(NULL, 1, 10);
    if (fix && v == 1)
        *vp = FIX;
    else if (var && (v < 5 || !row))
        *vp = VAR;
    else
        *vp = ROW;
}

/*
 * config_map_backup_incr --
 *     Map a incremental backup configuration to a flag.
 */
static void
config_map_backup_incr(const char *s, u_int *vp)
{
    if (strcmp(s, "block") == 0)
        *vp = INCREMENTAL_BLOCK;
    else if (strcmp(s, "log") == 0)
        *vp = INCREMENTAL_LOG;
    else if (strcmp(s, "off") == 0)
        *vp = INCREMENTAL_OFF;
    else
        testutil_die(EINVAL, "illegal incremental backup configuration: %s", s);
}

/*
 * config_map_checkpoint --
 *     Map a checkpoint configuration to a flag.
 */
static void
config_map_checkpoint(const char *s, u_int *vp)
{
    /* Checkpoint configuration used to be 1/0, let it continue to work. */
    if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0)
        *vp = CHECKPOINT_ON;
    else if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0)
        *vp = CHECKPOINT_OFF;
    else if (strcmp(s, "wiredtiger") == 0)
        *vp = CHECKPOINT_WIREDTIGER;
    else
        testutil_die(EINVAL, "illegal checkpoint configuration: %s", s);
}

/*
 * config_map_checksum --
 *     Map a checksum configuration to a flag.
 */
static void
config_map_checksum(const char *s, u_int *vp)
{
    if (strcmp(s, "on") == 0)
        *vp = CHECKSUM_ON;
    else if (strcmp(s, "off") == 0)
        *vp = CHECKSUM_OFF;
    else if (strcmp(s, "uncompressed") == 0)
        *vp = CHECKSUM_UNCOMPRESSED;
    else if (strcmp(s, "unencrypted") == 0)
        *vp = CHECKSUM_UNENCRYPTED;
    else
        testutil_die(EINVAL, "illegal checksum configuration: %s", s);
}

/*
 * config_map_compression --
 *     Map a compression configuration to a flag.
 */
static void
config_map_compression(const char *s, u_int *vp)
{
    if (strcmp(s, "none") == 0)
        *vp = COMPRESS_NONE;
    else if (strcmp(s, "lz4") == 0)
        *vp = COMPRESS_LZ4;
    else if (strcmp(s, "lz4-noraw") == 0) /* CONFIG compatibility */
        *vp = COMPRESS_LZ4;
    else if (strcmp(s, "snappy") == 0)
        *vp = COMPRESS_SNAPPY;
    else if (strcmp(s, "zlib") == 0)
        *vp = COMPRESS_ZLIB;
    else if (strcmp(s, "zlib-noraw") == 0) /* CONFIG compatibility */
        *vp = COMPRESS_ZLIB;
    else if (strcmp(s, "zstd") == 0)
        *vp = COMPRESS_ZSTD;
    else
        testutil_die(EINVAL, "illegal compression configuration: %s", s);
}

/*
 * config_map_encryption --
 *     Map a encryption configuration to a flag.
 */
static void
config_map_encryption(const char *s, u_int *vp)
{
    if (strcmp(s, "none") == 0)
        *vp = ENCRYPT_NONE;
    else if (strcmp(s, "rotn-7") == 0)
        *vp = ENCRYPT_ROTN_7;
    else if (strcmp(s, "sodium") == 0)
        *vp = ENCRYPT_SODIUM;
    else
        testutil_die(EINVAL, "illegal encryption configuration: %s", s);
}

/*
 * config_is_perm
 *	Return if a specific configuration entry was permanently set.
 */
static int
config_is_perm(const char *s)
{
    CONFIG *cp;

    cp = config_find(s, strlen(s), true);
    return (F_ISSET(cp, C_PERM) ? 1 : 0);
}

/*
 * config_file_type --
 *     Return the file type as a string.
 */
static const char *
config_file_type(u_int type)
{
    switch (type) {
    case FIX:
        return ("fixed-length column-store");
    case VAR:
        return ("variable-length column-store");
    case ROW:
        return ("row-store");
    default:
        break;
    }
    return ("error: unknown file type");
}
