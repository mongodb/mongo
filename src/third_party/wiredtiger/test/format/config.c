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

static void config_cache(void);
static void config_checkpoint(void);
static void config_checksum(void);
static void config_compression(const char *);
static void config_encryption(void);
static const char *config_file_type(u_int);
static bool config_fix(void);
static void config_in_memory(void);
static void config_in_memory_reset(void);
static int config_is_perm(const char *);
static void config_lrt(void);
static void config_lsm_reset(void);
static void config_map_checkpoint(const char *, u_int *);
static void config_map_checksum(const char *, u_int *);
static void config_map_compression(const char *, u_int *);
static void config_map_encryption(const char *, u_int *);
static void config_map_file_type(const char *, u_int *);
static void config_map_isolation(const char *, u_int *);
static void config_pct(void);
static void config_reset(void);
static void config_transaction(void);

/*
 * config_setup --
 *     Initialize configuration for a run.
 */
void
config_setup(void)
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
    if (!config_is_perm("file_type")) {
        if (config_is_perm("data_source") && DATASOURCE("lsm"))
            config_single("file_type=row", false);
        else
            switch (mmrand(NULL, 1, 10)) {
            case 1:
            case 2:
            case 3: /* 30% */
                config_single("file_type=var", false);
                break;
            case 4: /* 10% */
                if (config_fix()) {
                    config_single("file_type=fix", false);
                    break;
                }
                /* FALLTHROUGH */ /* 60% */
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
                config_single("file_type=row", false);
                break;
            }
    }
    config_map_file_type(g.c_file_type, &g.type);

    if (!config_is_perm("data_source")) {
        config_single("data_source=table", false);
        switch (mmrand(NULL, 1, 5)) {
        case 1: /* 20% */
            config_single("data_source=file", false);
            break;
        case 2: /* 20% */
                /*
                 * LSM requires a row-store and backing disk.
                 *
                 * Configuring truncation or timestamps results in LSM cache problems, don't
                 * configure LSM if those set.
                 *
                 * XXX Remove the timestamp test when WT-4162 resolved.
                 */
            if (g.type != ROW || g.c_in_memory)
                break;
            if (config_is_perm("transaction_timestamps") && g.c_txn_timestamps)
                break;
            if (config_is_perm("truncate") && g.c_truncate)
                break;
            config_single("data_source=lsm", false);
            break;
        case 3:
        case 4:
        case 5: /* 60% */
            break;
        }
    }

    /*
     * If data_source and file_type were both "permanent", we may still have a mismatch.
     */
    if (DATASOURCE("lsm") && g.type != ROW) {
        fprintf(stderr, "%s: lsm data_source is only compatible with row file_type\n", progname);
        exit(EXIT_FAILURE);
    }

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
        config_single("reverse=off", false);

    /* First, transaction configuration, it configures other features. */
    config_transaction();

    /* Simple selection. */
    config_checkpoint();
    config_checksum();
    config_compression("compression");
    config_compression("logging_compression");
    config_encryption();
    config_lrt();

    /* Configuration based on the configuration already chosen. */
    config_pct();
    config_cache();

    /* Give in-memory and LSM configurations a final review. */
    if (g.c_in_memory != 0)
        config_in_memory_reset();
    if (DATASOURCE("lsm"))
        config_lsm_reset();

    /*
     * Key/value minimum/maximum are related, correct unless specified by the configuration.
     */
    if (!config_is_perm("key_min") && g.c_key_min > g.c_key_max)
        g.c_key_min = g.c_key_max;
    if (!config_is_perm("key_max") && g.c_key_max < g.c_key_min)
        g.c_key_max = g.c_key_min;
    if (g.c_key_min > g.c_key_max)
        testutil_die(EINVAL, "key_min may not be larger than key_max");

    if (!config_is_perm("value_min") && g.c_value_min > g.c_value_max)
        g.c_value_min = g.c_value_max;
    if (!config_is_perm("value_max") && g.c_value_max < g.c_value_min)
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
    if (config_is_perm("timer")) {
        if (!config_is_perm("ops"))
            config_single("ops=0", false);
    } else {
        if (!config_is_perm("ops"))
            config_single("timer=30", false);
        else
            config_single("timer=360", false);
    }

    /* Reset the key count. */
    g.key_cnt = 0;
}

/*
 * config_cache --
 *     Cache configuration.
 */
static void
config_cache(void)
{
    uint32_t required;

    /* Page sizes are powers-of-two for bad historic reasons. */
    g.intl_page_max = 1U << g.c_intl_page_max;
    g.leaf_page_max = 1U << g.c_leaf_page_max;

    /* Check if a minimum cache size has been specified. */
    if (config_is_perm("cache")) {
        if (config_is_perm("cache_minimum") && g.c_cache_minimum != 0 &&
          g.c_cache < g.c_cache_minimum)
            testutil_die(EINVAL,
              "minimum cache set larger than cache "
              "(%" PRIu32 " > %" PRIu32 ")",
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
    g.c_cache = WT_MAX(g.c_cache, 2 * g.c_threads * g.c_memory_page_max);

    /*
     * Ensure cache size sanity for LSM runs. An LSM tree open requires 3
     * chunks plus a page for each participant in up to three concurrent
     * merges. Integrate a thread count into that calculation by requiring
     * 3 chunks/pages per configured thread. That might be overkill, but
     * LSM runs are more sensitive to small caches than other runs, and a
     * generous cache avoids stalls we're not interested in chasing.
     */
    if (DATASOURCE("lsm")) {
        required = WT_LSM_TREE_MINIMUM_SIZE(
          g.c_chunk_size * WT_MEGABYTE, g.c_threads * g.c_merge_max, g.c_threads * g.leaf_page_max);
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
    if (!config_is_perm("checkpoints"))
        switch (mmrand(NULL, 1, 20)) {
        case 1:
        case 2:
        case 3:
        case 4: /* 20% */
            config_single("checkpoints=wiredtiger", false);
            break;
        case 5: /* 5 % */
            config_single("checkpoints=off", false);
            break;
        default: /* 75% */
            config_single("checkpoints=on", false);
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
    if (!config_is_perm("checksum"))
        switch (mmrand(NULL, 1, 10)) {
        case 1: /* 10% */
            config_single("checksum=on", false);
            break;
        case 2: /* 10% */
            config_single("checksum=off", false);
            break;
        default: /* 80% */
            config_single("checksum=uncompressed", false);
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
    if (strcmp(conf_name, "logging_compression") == 0 && g.c_logging == 0) {
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
    if (!config_is_perm("encryption")) {
        cstr = "encryption=none";
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
            cstr = "encryption=rotn-7";
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
    /*
     * Fixed-length column stores don't support the lookaside table (so, no long running
     * transactions), or modify operations.
     */
    if (config_is_perm("long_running_txn"))
        return (false);
    if (config_is_perm("modify_pct"))
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
    if (config_is_perm("backups"))
        return;
    if (config_is_perm("checkpoints"))
        return;
    if (config_is_perm("compression"))
        return;
    if (config_is_perm("data_source") && DATASOURCE("lsm"))
        return;
    if (config_is_perm("logging"))
        return;
    if (config_is_perm("rebalance"))
        return;
    if (config_is_perm("salvage"))
        return;
    if (config_is_perm("verify"))
        return;

    if (!config_is_perm("in_memory") && mmrand(NULL, 1, 20) == 1)
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
    if (!config_is_perm("alter"))
        config_single("alter=off", false);
    if (!config_is_perm("backups"))
        config_single("backups=off", false);
    if (!config_is_perm("checkpoints"))
        config_single("checkpoints=off", false);
    if (!config_is_perm("compression"))
        config_single("compression=none", false);
    if (!config_is_perm("logging"))
        config_single("logging=off", false);
    if (!config_is_perm("rebalance"))
        config_single("rebalance=off", false);
    if (!config_is_perm("salvage"))
        config_single("salvage=off", false);
    if (!config_is_perm("verify"))
        config_single("verify=off", false);

    /*
     * Keep keys/values small, overflow items aren't an issue for in-memory configurations and it
     * keeps us from overflowing the cache.
     */
    if (!config_is_perm("key_max"))
        config_single("key_max=32", false);
    if (!config_is_perm("value_max"))
        config_single("value_max=80", false);

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
    if (!config_is_perm("truncate"))
        config_single("truncate=off", false);

    /*
     * LSM doesn't currently play nicely with timestamps, don't choose the pair unless forced to. If
     * we turn off timestamps, make sure we turn off prepare as well, it requires timestamps. Remove
     * this code with WT-4162.
     */
    if (!config_is_perm("prepare") && !config_is_perm("transaction_timestamps")) {
        config_single("prepare=off", false);
        config_single("transaction_timestamps=off", false);
    }
}

/*
 * config_lrt --
 *     Long-running transaction configuration.
 */
static void
config_lrt(void)
{
    /*
     * WiredTiger doesn't support a lookaside file for fixed-length column stores.
     */
    if (g.type == FIX && g.c_long_running_txn) {
        if (config_is_perm("long_running_txn"))
            testutil_die(EINVAL,
              "long_running_txn not supported with fixed-length "
              "column store");
        config_single("long_running_txn=off", false);
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
      {"delete_pct", &g.c_delete_pct, 0}, {"insert_pct", &g.c_insert_pct, 0},
#define CONFIG_MODIFY_ENTRY 2
      {"modify_pct", &g.c_modify_pct, 0}, {"read_pct", &g.c_read_pct, 0},
      {"write_pct", &g.c_write_pct, 0},
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
        if (config_is_perm("modify_pct") && g.c_modify_pct != 0)
            testutil_die(EINVAL,
              "WT_CURSOR.modify not supported by fixed-length "
              "column store");
        list[CONFIG_MODIFY_ENTRY].order = 0;
        *list[CONFIG_MODIFY_ENTRY].vp = 0;
    }

    /*
     * Cursor modify isn't possible for anything besides snapshot isolation transactions. If both
     * forced, it's an error. The run-time operations code converts modify operations into updates
     * if we're in some other transaction type, but if we're never going to be able to do a modify,
     * turn it off in the CONFIG output to avoid misleading debuggers.
     */
    if (g.c_isolation_flag == ISOLATION_READ_COMMITTED ||
      g.c_isolation_flag == ISOLATION_READ_UNCOMMITTED) {
        if (config_is_perm("isolation") && config_is_perm("modify_pct") && g.c_modify_pct != 0)
            testutil_die(EINVAL,
              "WT_CURSOR.modify only supported with "
              "snapshot isolation transactions");

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
 * config_transaction --
 *     Transaction configuration.
 */
static void
config_transaction(void)
{
    /*
     * We can't prepare a transaction if logging is configured or timestamps aren't configured.
     * Further, for repeatable reads to work in timestamp testing, all updates must be within a
     * snapshot-isolation transaction. Check for incompatible configurations, then let prepare and
     * timestamp drive the remaining configuration.
     */
    if (g.c_prepare) {
        if (config_is_perm("prepare")) {
            if (g.c_logging && config_is_perm("logging"))
                testutil_die(EINVAL, "prepare is incompatible with logging");
            if (!g.c_txn_timestamps && config_is_perm("transaction_timestamps"))
                testutil_die(EINVAL, "prepare requires transaction timestamps");
        } else if ((g.c_logging && config_is_perm("logging")) ||
          (!g.c_txn_timestamps && config_is_perm("transaction_timestamps")))
            config_single("prepare=off", false);
        if (g.c_prepare) {
            if (g.c_logging)
                config_single("logging=off", false);
            if (!g.c_txn_timestamps)
                config_single("transaction_timestamps=on", false);
        }
    }

    if (g.c_txn_timestamps) {
        if (config_is_perm("transaction_timestamps")) {
            if (g.c_isolation_flag != ISOLATION_SNAPSHOT && config_is_perm("isolation"))
                testutil_die(EINVAL,
                  "transaction_timestamps or prepare require "
                  "isolation=snapshot");
            if (g.c_txn_freq != 100 && config_is_perm("transaction-frequency"))
                testutil_die(EINVAL,
                  "transaction_timestamps or prepare require "
                  "transaction-frequency=100");
        } else if ((g.c_isolation_flag != ISOLATION_SNAPSHOT && config_is_perm("isolation")) ||
          (g.c_txn_freq != 100 && config_is_perm("transaction-frequency")))
            config_single("transaction_timestamps=off", false);
    }
    if (g.c_txn_timestamps) {
        if (g.c_isolation_flag != ISOLATION_SNAPSHOT)
            config_single("isolation=snapshot", false);
        if (g.c_txn_freq != 100)
            config_single("transaction-frequency=100", false);
    } else if (!config_is_perm("isolation"))
        switch (mmrand(NULL, 1, 4)) {
        case 1:
            config_single("isolation=random", false);
            break;
        case 2:
            config_single("isolation=read-uncommitted", false);
            break;
        case 3:
            config_single("isolation=read-committed", false);
            break;
        case 4:
        default:
            config_single("isolation=snapshot", false);
            break;
        }
}

/*
 * config_error --
 *     Display configuration information on error.
 */
void
config_error(void)
{
    CONFIG *cp;

    /* Display configuration names. */
    fprintf(stderr, "\n");
    fprintf(stderr, "Configuration names:\n");
    for (cp = c; cp->name != NULL; ++cp)
        if (strlen(cp->name) > 17)
            fprintf(stderr, "%s\n%17s: %s\n", cp->name, " ", cp->desc);
        else
            fprintf(stderr, "%17s: %s\n", cp->name, cp->desc);
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

    if (error_display)
        fp = stdout;
    else if ((fp = fopen(g.home_config, "w")) == NULL)
        testutil_die(errno, "fopen: %s", g.home_config);

    fprintf(fp, "############################################\n");
    fprintf(fp, "#  RUN PARAMETERS\n");
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
    char buf[256], *p;

    if ((fp = fopen(name, "r")) == NULL)
        testutil_die(errno, "fopen: %s", name);
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        for (p = buf; *p != '\0' && *p != '\n'; ++p)
            ;
        *p = '\0';
        if (buf[0] == '\0' || buf[0] == '#')
            continue;
        config_single(buf, true);
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

    /*
     * Optionally ignore unknown keywords, it makes it easier to run old CONFIG files.
     */
    if (fatal) {
        fprintf(stderr, "%s: %s: unknown required configuration keyword\n", progname, s);
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "%s: %s: WARNING, ignoring unknown configuration keyword\n", progname, s);
    return (NULL);
}

/*
 * config_single --
 *     Set a single configuration structure value.
 */
void
config_single(const char *s, bool perm)
{
    CONFIG *cp;
    long vlong;
    uint32_t v;
    char *p;
    const char *ep;

    if ((ep = strchr(s, '=')) == NULL) {
        fprintf(stderr, "%s: %s: illegal configuration value\n", progname, s);
        exit(EXIT_FAILURE);
    }

    if ((cp = config_find(s, (size_t)(ep - s), false)) == NULL)
        return;

    F_SET(cp, perm ? C_PERM : C_TEMP);
    ++ep;

    if (F_ISSET(cp, C_STRING)) {
        /*
         * Free the previous setting if a configuration has been passed in twice.
         */
        if (*cp->vstr != NULL) {
            free(*cp->vstr);
            *cp->vstr = NULL;
        }

        if (strncmp(s, "checkpoints", strlen("checkpoints")) == 0) {
            config_map_checkpoint(ep, &g.c_checkpoint_flag);
            *cp->vstr = dstrdup(ep);
        } else if (strncmp(s, "checksum", strlen("checksum")) == 0) {
            config_map_checksum(ep, &g.c_checksum_flag);
            *cp->vstr = dstrdup(ep);
        } else if (strncmp(s, "compression", strlen("compression")) == 0) {
            config_map_compression(ep, &g.c_compression_flag);
            *cp->vstr = dstrdup(ep);
        } else if (strncmp(s, "data_source", strlen("data_source")) == 0 &&
          strncmp("file", ep, strlen("file")) != 0 && strncmp("lsm", ep, strlen("lsm")) != 0 &&
          strncmp("table", ep, strlen("table")) != 0) {
            fprintf(stderr, "Invalid data source option: %s\n", ep);
            exit(EXIT_FAILURE);
        } else if (strncmp(s, "encryption", strlen("encryption")) == 0) {
            config_map_encryption(ep, &g.c_encryption_flag);
            *cp->vstr = dstrdup(ep);
        } else if (strncmp(s, "file_type", strlen("file_type")) == 0) {
            config_map_file_type(ep, &g.type);
            *cp->vstr = dstrdup(config_file_type(g.type));
        } else if (strncmp(s, "isolation", strlen("isolation")) == 0) {
            config_map_isolation(ep, &g.c_isolation_flag);
            *cp->vstr = dstrdup(ep);
        } else if (strncmp(s, "logging_compression", strlen("logging_compression")) == 0) {
            config_map_compression(ep, &g.c_logging_compression_flag);
            *cp->vstr = dstrdup(ep);
        } else
            *cp->vstr = dstrdup(ep);

        return;
    }

    vlong = -1;
    if (F_ISSET(cp, C_BOOL)) {
        if (strncmp(ep, "off", strlen("off")) == 0)
            vlong = 0;
        else if (strncmp(ep, "on", strlen("on")) == 0)
            vlong = 1;
    }
    if (vlong == -1) {
        vlong = strtol(ep, &p, 10);
        if (*p != '\0') {
            fprintf(stderr, "%s: %s: illegal numeric value\n", progname, s);
            exit(EXIT_FAILURE);
        }
    }
    v = (uint32_t)vlong;
    if (F_ISSET(cp, C_BOOL)) {
        if (v != 0 && v != 1) {
            fprintf(stderr, "%s: %s: value of boolean not 0 or 1\n", progname, s);
            exit(EXIT_FAILURE);
        }
    } else if (v < cp->min || v > cp->maxset) {
        fprintf(stderr, "%s: %s: value outside min/max values of %" PRIu32 "-%" PRIu32 "\n",
          progname, s, cp->min, cp->maxset);
        exit(EXIT_FAILURE);
    }

    *cp->v = v;
}

/*
 * config_map_file_type --
 *     Map a file type configuration to a flag.
 */
static void
config_map_file_type(const char *s, u_int *vp)
{
    if (strcmp(s, "fix") == 0 || strcmp(s, "fixed-length column-store") == 0)
        *vp = FIX;
    else if (strcmp(s, "var") == 0 || strcmp(s, "variable-length column-store") == 0)
        *vp = VAR;
    else if (strcmp(s, "row") == 0 || strcmp(s, "row-store") == 0)
        *vp = ROW;
    else
        testutil_die(EINVAL, "illegal file type configuration: %s", s);
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
        *vp = CHECKSUM_ON;
    else if (strcmp(s, "uncompressed") == 0)
        *vp = CHECKSUM_UNCOMPRESSED;
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
    else
        testutil_die(EINVAL, "illegal encryption configuration: %s", s);
}

/*
 * config_map_isolation --
 *     Map an isolation configuration to a flag.
 */
static void
config_map_isolation(const char *s, u_int *vp)
{
    if (strcmp(s, "random") == 0)
        *vp = ISOLATION_RANDOM;
    else if (strcmp(s, "read-uncommitted") == 0)
        *vp = ISOLATION_READ_UNCOMMITTED;
    else if (strcmp(s, "read-committed") == 0)
        *vp = ISOLATION_READ_COMMITTED;
    else if (strcmp(s, "snapshot") == 0)
        *vp = ISOLATION_SNAPSHOT;
    else
        testutil_die(EINVAL, "illegal isolation configuration: %s", s);
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
