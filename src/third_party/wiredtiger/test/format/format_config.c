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

static void config_backup_incr(void);
static void config_backup_incr_granularity(void);
static void config_backward_compatible(void);
static void config_cache(void);
static void config_checkpoint(void);
static void config_checksum(TABLE *);
static void config_chunk_cache(void);
static void config_compact(void);
static void config_compression(TABLE *, const char *);
static void config_disagg_storage(void);
static void config_encryption(void);
static bool config_explicit(TABLE *, const char *);
static const char *config_file_type(u_int);
static bool config_fix(TABLE *);
static void config_in_memory(void);
static void config_in_memory_reset(void);
static void config_map_backup_incr(const char *, bool *);
static void config_map_checkpoint(const char *, u_int *);
static void config_map_file_type(const char *, u_int *);
static void config_mirrors(void);
static void config_mirrors_disable_reverse(void);
static void config_obsolete_cleanup(void);
static void config_off(TABLE *, const char *);
static void config_off_all(const char *);
static void config_pct(TABLE *);
static void config_run_length(void);
static void config_statistics(void);
static void config_tiered_storage(void);
static void config_transaction(void);
static bool config_var(TABLE *);

/*
 * config_random_generator --
 *     For a given seed/RNG combination, generate a seed if not given, and initialize the RNG.
 */
static void
config_random_generator(
  const char *config_name, uint64_t seed, uint32_t rand_count, WT_RAND_STATE *rnd)
{
    char buf[128];
    bool seed_set;

    /* See if the seed is already present in the configuration. */
    seed_set = (seed != 0);

    /* Initialize the RNG, and potentially the seed. */
    testutil_random_init(rnd, &seed, rand_count);

    /* If we generated a seed just now, put it into the configuration file. */
    if (!seed_set) {
        testutil_assert(seed != 0);
        testutil_snprintf(buf, sizeof(buf), "%s=%" PRIu64, config_name, seed);
        config_single(NULL, buf, true);
    }

    /* Make sure the generator is ready. */
    testutil_assert(rnd->v != 0);
}

/*
 * config_random_generators --
 *     Initialize our global random generators using provided seeds.
 */
static void
config_random_generators(void)
{
    config_random_generator("random.data_seed", GV(RANDOM_DATA_SEED), 0, &g.data_rnd);
    config_random_generator("random.extra_seed", GV(RANDOM_EXTRA_SEED), 1, &g.extra_rnd);
}

/*
 * config_random_generators_before_run --
 *     One use case for predictable replay is to run test/format once with little or no
 *     configuration values set. test/format rolls the dice and picks the configuration, recording
 *     it along with the random seeds. If we want to rerun it predictably, we can use the same
 *     seeds. However, the second run will not need to roll the dice during configuration, so the
 *     state of the RNG after configuration would be different than after configuration during the
 *     first run. To make everything line up, we re-seed the generator after the configuration, and
 *     before execution begins.
 */
static void
config_random_generators_before_run(void)
{
    testutil_random_from_seed(&g.data_rnd, GV(RANDOM_DATA_SEED));
    testutil_random_from_seed(&g.extra_rnd, GV(RANDOM_EXTRA_SEED));
}

/*
 * config_random --
 *     Do random configuration on the remaining global or table space.
 */
static void
config_random(TABLE *table, bool table_only)
{
    CONFIG *cp;
    CONFIGV *v;
    char buf[128];

    for (cp = configuration_list; cp->name != NULL; ++cp) {
        if (F_ISSET(cp, C_IGNORE))
            continue;
        if (table_only && !F_ISSET(cp, C_TABLE))
            continue;
        if (!table_only && F_ISSET(cp, C_TABLE))
            continue;

        /*
         * Don't randomly configure runs.tables if we read a CONFIG file, that prevents us from
         * turning old-style CONFIG files into multi-table tests.
         */
        if (cp->off == V_GLOBAL_RUNS_TABLES && !g.multi_table_config)
            continue;

        v = &table->v[cp->off];
        if (v->set)
            continue;

        /* Configure key prefixes only rarely, 5% if the length isn't set explicitly. */
        if (cp->off == V_TABLE_BTREE_PREFIX_LEN && mmrand(&g.extra_rnd, 1, 100) > 5)
            continue;

        /*
         * Boolean flags are 0 or 1, where the variable's "min" value is the percent chance the flag
         * is "on" (so "on" if random rolled <= N, otherwise "off").
         */
        if (F_ISSET(cp, C_BOOL))
            testutil_snprintf(buf, sizeof(buf), "%s=%s", cp->name,
              mmrand(&g.data_rnd, 1, 100) <= cp->min ? "on" : "off");
        else if (F_ISSET(cp, C_POW2)) {
            double max, min;
            uint32_t vbits, val_p2;

            max = log2((double)cp->maxrand);
            testutil_assert(max < 32);
            min = log2((double)cp->min);
            vbits = mmrand(&g.data_rnd, (uint32_t)min, (uint32_t)max);
            val_p2 = (uint32_t)(1 << vbits);
            testutil_snprintf(buf, sizeof(buf), "%s=%" PRIu32, cp->name, val_p2);
        } else
            testutil_snprintf(
              buf, sizeof(buf), "%s=%" PRIu32, cp->name, mmrand(&g.data_rnd, cp->min, cp->maxrand));
        config_single(table, buf, false);
    }
}

/*
 * config_promote --
 *     Promote a base value to a table.
 */
static void
config_promote(TABLE *table, CONFIG *cp, CONFIGV *v)
{
    char buf[128];

    if (F_ISSET(cp, C_STRING))
        testutil_snprintf(buf, sizeof(buf), "%s=%s", cp->name, v->vstr);
    else
        testutil_snprintf(buf, sizeof(buf), "%s=%" PRIu32, cp->name, v->v);
    config_single(table, buf, true);
}

/*
 * config_table_am --
 *     Configure the table's access methods (type and source).
 */
static void
config_table_am(TABLE *table)
{
    char buf[128];

    /*
     * The runs.type configuration allows more than a single type, for example, choosing from either
     * RS and VLCS but not FLCS. If there's no table value but there was a global value, re-evaluate
     * the original global specification, not the choice set for the global table.
     */
    if (!table->v[V_TABLE_RUNS_TYPE].set && tables[0]->v[V_TABLE_RUNS_TYPE].set) {
        testutil_snprintf(buf, sizeof(buf), "runs.type=%s", g.runs_type);
        config_single(table, buf, true);
    }

    if (!config_explicit(table, "runs.type")) {
        if (config_explicit(table, "runs.source") && DATASOURCE(table, "layered"))
            config_single(table, "runs.type=row", false);
        else
            switch (mmrand(&g.data_rnd, 1, 10)) {
            case 1:
            case 2:
            case 3: /* 30% */
                if (config_var(table)) {
                    config_single(table, "runs.type=var", false);
                    break;
                }
                /* FALLTHROUGH */
            case 4: /* 10% */
                if (config_fix(table)) {
                    config_single(table, "runs.type=fix", false);
                    break;
                }
                /* FALLTHROUGH */ /* 60% */
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
                config_single(table, "runs.type=row", false);
                break;
            }
    }

    if (!config_explicit(table, "runs.source"))
        switch (mmrand(&g.data_rnd, 1, 5)) {
        case 1: /* 20% */
            config_single(table, "runs.source=file", false);
            break;
        case 2: /* 20% */
            /* FALLTHROUGH */
        case 3:
        case 4:
        case 5: /* 60% */
            config_single(table, "runs.source=table", false);
            break;
        }
}

/*
 * config_table --
 *     Finish initialization of a single table.
 */
static void
config_table(TABLE *table, void *arg)
{
    CONFIG *cp;

    (void)arg; /* unused argument */

    testutil_assert(table != NULL);

    /*
     * Choose a file format and a data source: they're interrelated and other items depend on them.
     */
    config_table_am(table);

    /*
     * Next, for any values set in the base configuration, export them to this table (where this
     * table doesn't already have a value set). This is done after picking an access method as the
     * access method is more complicated, the base value might be set to "row,var", to pick from two
     * possible access methods, and so we do that before blindly taking any already set values from
     * the base configuration. Also, don't copy the mirror setting, it's more complicated as well.
     */
    if (ntables != 0)
        for (cp = configuration_list; cp->name != NULL; ++cp)
            if (F_ISSET(cp, C_TABLE) && cp->off != V_TABLE_RUNS_MIRROR && !table->v[cp->off].set &&
              tables[0]->v[cp->off].set)
                config_promote(table, cp, &tables[0]->v[cp->off]);

    /*
     * Build the top-level object name: we're overloading data_source in our configuration.
     */
    if (ntables == 0)
        testutil_snprintf(
          table->uri, sizeof(table->uri), "%s", DATASOURCE(table, "file") ? "file:wt" : "table:wt");
    else
        testutil_snprintf(table->uri, sizeof(table->uri),
          DATASOURCE(table, "file") ? "file:F%05u" : "table:T%05u", table->id);
    testutil_snprintf(table->track_prefix, sizeof(table->track_prefix), "table %u", table->id);

    /* Fill in random values for the rest of the run. */
    config_random(table, true);

    /* Page sizes are configured using powers-of-two or megabytes, convert them. */
    table->max_intl_page = 1U << TV(BTREE_INTERNAL_PAGE_MAX);
    table->max_leaf_page = 1U << TV(BTREE_LEAF_PAGE_MAX);
    table->max_mem_page = MEGABYTE(TV(BTREE_MEMORY_PAGE_MAX));

    /*
     * Keep the number of rows and keys/values small for in-memory (overflow items aren't an issue
     * for in-memory configurations and it helps prevents cache overflow).
     */
    if (GV(RUNS_IN_MEMORY)) {
        /*
         * Always limit the row count if it's greater than one million and in memory wasn't
         * explicitly set.
         */
        if (GV(RUNS_IN_MEMORY) && TV(RUNS_ROWS) > WT_MILLION &&
          config_explicit(NULL, "runs.in_memory")) {
            WARN("limiting table%" PRIu32
                 ".runs.rows to %d as runs.in_memory has been automatically enabled",
              table->id, WT_MILLION);
            config_single(table, "runs.rows=" XSTR(WT_MILLION_LITERAL), false);
        }
        if (!config_explicit(table, "btree.key_max"))
            config_single(table, "btree.key_max=32", false);
        if (!config_explicit(table, "btree.key_min"))
            config_single(table, "btree.key_min=15", false);
        if (!config_explicit(table, "btree.value_max"))
            config_single(table, "btree.value_max=80", false);
        if (!config_explicit(table, "btree.value_min"))
            config_single(table, "btree.value_min=20", false);
    }

    /*
     * Limit the rows to one million if the realloc exact and realloc malloc configs are on and not
     * all explicitly set. Realloc exact config allocates the exact amount of memory, which causes a
     * new realloc call every time we append to an array. Realloc malloc turns a single realloc call
     * to a malloc, a memcpy, and a free. The combination of both will significantly slow the
     * execution.
     */
    if ((!config_explicit(NULL, "debug.realloc_exact") ||
          !config_explicit(NULL, "debug.realloc_malloc")) &&
      GV(DEBUG_REALLOC_EXACT) && GV(DEBUG_REALLOC_MALLOC) && TV(RUNS_ROWS) > WT_MILLION) {
        config_single(table, "runs.rows=" XSTR(WT_MILLION_LITERAL), true);
        WARN("limiting table%" PRIu32
             ".runs.rows to %d if realloc_exact or realloc_malloc has been automatically set",
          table->id, WT_MILLION);
    }

#ifndef WT_STANDALONE_BUILD
    /*
     * Non-standalone builds do not support writing fast truncate information to disk, as this
     * information is required to rollback any unstable fast truncate operation.
     *
     * To avoid this problem to occur during the test, disable the truncate operation whenever
     * timestamp or prepare is enabled.
     */
    if (GV(TRANSACTION_TIMESTAMPS) || config_explicit(NULL, "transaction.timestamps") ||
      GV(OPS_PREPARE) || config_explicit(NULL, "ops.prepare"))
        config_off(table, "ops.truncate");
#endif

    /*
     * Key/value minimum/maximum are related, correct unless specified by the configuration. Key
     * sizes are a row-store consideration: column-store doesn't store keys, a constant of 8 will
     * reserve a small amount of additional space.
     */
    if (table->type == ROW) {
        if (!config_explicit(table, "btree.key_min") && TV(BTREE_KEY_MIN) > TV(BTREE_KEY_MAX))
            TV(BTREE_KEY_MIN) = TV(BTREE_KEY_MAX);
        if (!config_explicit(table, "btree.key_max") && TV(BTREE_KEY_MAX) < TV(BTREE_KEY_MIN))
            TV(BTREE_KEY_MAX) = TV(BTREE_KEY_MIN);
        if (TV(BTREE_KEY_MIN) > TV(BTREE_KEY_MAX))
            testutil_die(EINVAL, "btree.key_min may not be larger than btree.key_max");
    } else
        TV(BTREE_KEY_MIN) = TV(BTREE_KEY_MAX) = 8;
    if (!config_explicit(table, "btree.value_min") && TV(BTREE_VALUE_MIN) > TV(BTREE_VALUE_MAX))
        TV(BTREE_VALUE_MIN) = TV(BTREE_VALUE_MAX);
    if (!config_explicit(table, "btree.value_max") && TV(BTREE_VALUE_MAX) < TV(BTREE_VALUE_MIN))
        TV(BTREE_VALUE_MAX) = TV(BTREE_VALUE_MIN);
    if (TV(BTREE_VALUE_MIN) > TV(BTREE_VALUE_MAX))
        testutil_die(EINVAL, "btree.value_min may not be larger than btree.value_max");

    if (GV(RUNS_PREDICTABLE_REPLAY)) {
        /*
         * In predictable replay, force the number of rows in a table to be a manageable size so we
         * can modify key numbers without problems.
         */
        TV(RUNS_ROWS) = WT_MAX(TV(RUNS_ROWS), 2 * LANE_COUNT);

        /*
         * We don't support some operations in predictable replay.
         */
        if (!replay_operation_enabled(MODIFY)) {
            if (config_explicit(table, "ops.pct.modify") && TV(OPS_PCT_MODIFY))
                WARN("turning off modify operations for table%" PRIu32
                     " to work with predictable replay",
                  table->id);
            config_single(table, "ops.pct.modify=0", false);
        }
        if (!replay_operation_enabled(TRUNCATE)) {
            if (config_explicit(table, "ops.truncate") && TV(OPS_TRUNCATE))
                WARN("turning off truncate for table%" PRIu32 " to work with predictable replay",
                  table->id);
            config_single(table, "ops.truncate=0", false);
        }

        /*
         * We don't support the hs_search stress point with pareto distribution in predictable
         * replay as it prevents us stopping in time.
         */
        if (GV(STRESS_HS_SEARCH) && TV(OPS_PARETO)) {
            if (config_explicit(NULL, "stress.hs_search"))
                WARN("turning off stress.hs_search to work with predictable replay as table%" PRIu32
                     " has pareto enabled",
                  table->id);
            config_single(NULL, "stress.hs_search=0", false);
        }
    }

    /*
     * If common key prefixes are configured, add prefix compression if no explicit choice was made
     * and track the largest common key prefix in the run.
     */
    if (TV(BTREE_PREFIX_LEN) != 0) {
        if (TV(BTREE_PREFIX_COMPRESSION) == 0 &&
          !config_explicit(table, "btree.prefix_compression"))
            config_single(table, "btree.prefix_compression=on", false);
        g.prefix_len_max = WT_MAX(g.prefix_len_max, TV(BTREE_PREFIX_LEN));
    }

    config_checksum(table);
    config_compression(table, "btree.compression");
    config_pct(table);

    /* Column-store tables require special row insert resolution. */
    if (table->type != ROW) {
        g.column_store_config = true;
        /* FIXME-WT-15274 Support column store with precise checkpoint */
        if (GV(PRECISE_CHECKPOINT)) {
            if (config_explicit(NULL, "precise_checkpoint"))
                WARN("turning off precise_checkpoint as table%" PRIu32 " is a column-store",
                  table->id);
            config_off(NULL, "precise_checkpoint");
        }
    }

    /* Only row-store tables support a collation order. */
    if (table->type != ROW)
        config_off(table, "btree.reverse");
}

/*
 * config_run --
 *     Run initialization.
 */
void
config_run(void)
{
    config_random_generators(); /* Configure the random number generators. */

    config_random(tables[0], false); /* Configure the remaining global name space. */

    /*
     * Limit the number of tables to REALLOC_MAX_TABLES if realloc exact and realloc malloc are both
     * on and not all explicitly set to reduce the running time to an acceptable level.
     */
    if ((!config_explicit(NULL, "debug.realloc_exact") ||
          !config_explicit(NULL, "debug.realloc_malloc")) &&
      GV(DEBUG_REALLOC_EXACT) && GV(DEBUG_REALLOC_MALLOC) && ntables > REALLOC_MAX_TABLES) {
        ntables = REALLOC_MAX_TABLES;
        /*
         * The following config_single has no effect. It is just to overwrite the config in memory
         * so that we can dump the correct config.
         */
        config_single(NULL, "runs.tables=" XSTR(REALLOC_MAX_TABLES), true);
        WARN(
          "limiting runs.tables to %d if realloc_exact or realloc_malloc has been automatically "
          "set",
          REALLOC_MAX_TABLES);
    }

    if (GV(RUNS_PREDICTABLE_REPLAY)) {
        /*
         * Predictable replays can get extremely slow with throttling.
         *
         * FIXME-WT-11782: Investigate why predictable replays get stuck with ops.throttling
         * enabled. It can indicate a bug in predictable replay or in WiredTiger.
         */
        if (GV(OPS_THROTTLE)) {
            if (config_explicit(NULL, "ops.throttle"))
                WARN("%s", "turning off ops.throttle to work with predictable replay");
            config_single(NULL, "ops.throttle=0", false);
        }
    }

    config_in_memory(); /* Periodically run in-memory. */

    tables_apply(config_table, NULL); /* Configure the tables. */

    /* TODO: Temporarily disable salvage test due to increased failures. */
    config_off(NULL, "ops.salvage");

    /* Order can be important, don't shuffle without careful consideration. */
    config_tiered_storage();                         /* Tiered storage */
    config_disagg_storage();                         /* Disaggregated storage */
    config_chunk_cache();                            /* Chunk cache */
    config_transaction();                            /* Transactions */
    config_backup_incr();                            /* Incremental backup */
    config_checkpoint();                             /* Checkpoints */
    config_compression(NULL, "logging.compression"); /* Logging compression */
    config_encryption();                             /* Encryption */
    config_in_memory_reset();                        /* Reset in-memory as needed */
    config_backward_compatible();                    /* Reset backward compatibility as needed */
    config_mirrors();                                /* Mirrors */
    config_statistics();                             /* Statistics */
    config_compact();                                /* Compaction */
    config_obsolete_cleanup();                       /* Obsolete cleanup */

    /* Configure the cache last, cache size depends on everything else. */
    config_cache();

    /* Adjust run length if needed. */
    config_run_length();

    config_random_generators_before_run();
}

/*
 * config_backup_incr --
 *     Incremental backup configuration.
 */
static void
config_backup_incr(void)
{
    if (GV(BACKUP) == 0) {
        config_off(NULL, "backup.incremental");
        return;
    }

    /*
     * Incremental backup using log files is incompatible with automatic log removals. Testing log
     * file removal doesn't seem as useful as testing backup, let the backup configuration override.
     */
    if (config_explicit(NULL, "backup.incremental")) {
        if (g.backup_incr)
            config_backup_incr_granularity();
        return;
    }

    /*
     * Choose a type of incremental backup, where the log remove setting can eliminate incremental
     * backup based on log files.
     */
    switch (mmrand(&g.extra_rnd, 1, 5)) {
    case 1: /* 40% full backup only */
    case 2:
        config_off(NULL, "backup.incremental");
        break;
    case 3: /* 60% block based incremental */
    case 4:
    case 5:
        config_single(NULL, "backup.incremental=block", false);
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

    if (config_explicit(NULL, "backup.incr_granularity"))
        return;

    /*
     * Three block sizes are interesting. 16MB is the default for WiredTiger and MongoDB. 1MB is the
     * minimum allowed by MongoDB. Smaller sizes stress block tracking and are good for testing. The
     * granularity is in units of KB.
     */
    granularity = 0;
    i = mmrand(&g.extra_rnd, 1, 10);
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

    testutil_snprintf(confbuf, sizeof(confbuf), "backup.incr_granularity=%" PRIu32, granularity);
    config_single(NULL, confbuf, false);
}

/*
 * config_backward_compatible_table --
 *     Backward compatibility configuration, per table.
 */
static void
config_backward_compatible_table(TABLE *table, void *arg)
{
    (void)arg; /* unused argument */
    testutil_assert(table != NULL);

#undef BC_CHECK
#define BC_CHECK(name, flag)                                                               \
    if (TV(flag)) {                                                                        \
        if (config_explicit(table, name))                                                  \
            testutil_die(EINVAL, "%s not supported in backward compatibility mode", name); \
        config_off(table, name);                                                           \
    }
    BC_CHECK("btree.prefix_len", BTREE_PREFIX_LEN);
}

/*
 * config_backward_compatible --
 *     Backward compatibility configuration.
 */
static void
config_backward_compatible(void)
{
    /*
     * If built in a branch that doesn't support all current options, or creating a database for
     * such an environment, strip out configurations that won't work.
     */
    if (!g.backward_compatible)
        return;

#undef BC_CHECK
#define BC_CHECK(name, flag)                                                               \
    if (GV(flag)) {                                                                        \
        if (config_explicit(NULL, name))                                                   \
            testutil_die(EINVAL, "%s not supported in backward compatibility mode", name); \
        config_off(NULL, name);                                                            \
    }

    BC_CHECK("disk.mmap_all", DISK_MMAP_ALL);
    BC_CHECK("block_cache", BLOCK_CACHE);
    BC_CHECK("stress.hs_checkpoint_delay", STRESS_HS_CHECKPOINT_DELAY);
    BC_CHECK("stress.hs_search", STRESS_HS_SEARCH);
    BC_CHECK("stress.hs_sweep", STRESS_HS_SWEEP);

    tables_apply(config_backward_compatible_table, NULL);
}

/*
 * config_cache --
 *     Cache configuration.
 */
static void
config_cache(void)
{
    uint64_t cache, workers;
    bool cache_maximum_explicit;

    /* The maximum cache is only set if it is non-zero and explicitly set. */
    cache_maximum_explicit = GV(CACHE_MAXIMUM) != 0 && config_explicit(NULL, "cache.maximum");

    /* Sum the number of workers. */
    workers = GV(RUNS_THREADS);
    if (GV(OPS_HS_CURSOR))
        ++workers;
    if (GV(OPS_RANDOM_CURSOR))
        ++workers;

    /* Check if both min and max cache sizes have been specified and if they're consistent. */
    if (config_explicit(NULL, "cache")) {
        if (GV(CACHE) < 4086) {
            config_off(NULL, "preserve_prepared");
            config_off(NULL, "precise_checkpoint");
        }
        if (config_explicit(NULL, "cache.minimum") && GV(CACHE) < GV(CACHE_MINIMUM))
            testutil_die(EINVAL, "minimum cache set larger than cache (%" PRIu32 " > %" PRIu32 ")",
              GV(CACHE_MINIMUM), GV(CACHE));
        if (cache_maximum_explicit && GV(CACHE) > GV(CACHE_MAXIMUM))
            testutil_die(EINVAL, "cache set larger than maximum cache (%" PRIu32 " > %" PRIu32 ")",
              GV(CACHE), GV(CACHE_MAXIMUM));
        goto dirty_eviction_config;
    }

    /*
     * If the min and max cache have been explicitly set, we need to check that the min cache size
     * is less than or equal to the max cache size.
     */
    if (config_explicit(NULL, "cache.minimum") && cache_maximum_explicit &&
      GV(CACHE_MINIMUM) > GV(CACHE_MAXIMUM))
        testutil_die(EINVAL,
          "configured minimum cache set larger than cache maximum (%" PRIu32 " > %" PRIu32 ")",
          GV(CACHE_MINIMUM), GV(CACHE_MAXIMUM));

    GV(CACHE) = GV(CACHE_MINIMUM);

    /*
     * If it's an in-memory run or disaggregated follower mode, size the cache at 2x the maximum
     * initial data set. This calculation is done in bytes, convert to megabytes before testing
     * against the cache.
     */
    if (GV(RUNS_IN_MEMORY) ||
      (g.disagg_storage_config && strcmp(GVS(DISAGG_MODE), "follower") == 0)) {
        cache = table_sumv(V_TABLE_BTREE_KEY_MAX) + table_sumv(V_TABLE_BTREE_VALUE_MAX);
        cache *= table_sumv(V_TABLE_RUNS_ROWS);
        cache *= 2;
        cache /= WT_MEGABYTE; /* NOT in MB units, convert for cache test */
        if (GV(CACHE) < cache)
            GV(CACHE) = (uint32_t)cache;
    }

    /*
     * Maximum internal/leaf page size sanity.
     *
     * Ensure we can service at least one operation per-thread concurrently without filling the
     * cache with pinned pages, that is, every thread consuming an internal page and a leaf page (or
     * a pair of leaf pages for cursor movements).
     *
     * This code is what dramatically increases the cache size when there are lots of threads, it
     * grows the cache to several megabytes per thread.
     */
    cache = table_sumv(V_TABLE_BTREE_MEMORY_PAGE_MAX); /* in MB units, no conversion to cache */
    cache *= workers;
    cache *= 2;

    if (GV(PRECISE_CHECKPOINT))
        cache *= 6;

    if (GV(CACHE) < cache)
        GV(CACHE) = (uint32_t)cache;

    if (GV(PRECISE_CHECKPOINT) && GV(CACHE) < 4086)
        GV(CACHE) = 4086;

    if (cache_maximum_explicit && GV(CACHE) > GV(CACHE_MAXIMUM)) {
        if (GV(PRECISE_CHECKPOINT) && GV(CACHE_MAXIMUM) < 4086)
            config_off(NULL, "cache.maximum");
        else
            GV(CACHE) = GV(CACHE_MAXIMUM);
    }

    /* Give any block cache 20% of the total cache size, over and above the cache. */
    if (GV(BLOCK_CACHE) != 0)
        GV(BLOCK_CACHE_SIZE) = (GV(CACHE) + 4) / 5;

dirty_eviction_config:
    /* Adjust the dirty eviction settings to reduce test driven cache stuck failures. */
    if (GV(CACHE) < 20) {
        WARN("%s", "Setting cache.eviction_dirty_trigger=95 due to micro cache");
        config_single(NULL, "cache.eviction_dirty_trigger=95", false);
    } else if (GV(CACHE) / workers <= 2 && !config_explicit(NULL, "cache.eviction_dirty_trigger")) {
        WARN("Cache is minimally configured (%" PRIu32
             "mb), setting cache.eviction_dirty_trigger=40 and "
             "cache.eviction_dirty_target=10",
          GV(CACHE));
        config_single(NULL, "cache.eviction_dirty_trigger=40", false);
        config_single(NULL, "cache.eviction_dirty_target=10", false);
    }

    if (g.disagg_storage_config && strcmp(GVS(DISAGG_MODE), "follower") == 0) {
        WARN("%s",
          "Setting cache.eviction_dirty_trigger=95 and cache.eviction_update_trigger=95. In "
          "disaggregated follower mode, these eviction trigger thresholds are increased to help "
          "avoid operation thread stalls.");
        config_single(NULL, "cache.eviction_dirty_trigger=95", false);
        config_single(NULL, "cache.eviction_updates_trigger=95", false);
    }

    if (GV(PRECISE_CHECKPOINT) && GV(CACHE) < 4086) {
        WARN("%s", "Setting cache to minimum of 4086MB due to precise_checkpoint");
        config_single(NULL, "cache=4086", false);
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
    if (!config_explicit(NULL, "checkpoint"))
        switch (mmrand(&g.extra_rnd, 1, 20)) {
        case 1:
        case 2:
        case 3:
        case 4: /* 20% */
            config_single(NULL, "checkpoint=wiredtiger", false);
            break;
        case 5: /* 5% */
            config_off(NULL, "checkpoint");
            break;
        default: /* 75% */
            config_single(NULL, "checkpoint=on", false);
            /* 50% */
            if (mmrand(&g.extra_rnd, 1, 10) > 5)
                config_single(NULL, "checkpoint=on", false);
            break;
        }

    if (!GV(PRECISE_CHECKPOINT))
        config_off(NULL, "preserve_prepared");
}

/*
 * config_checksum --
 *     Checksum configuration.
 */
static void
config_checksum(TABLE *table)
{
    /* Choose a checksum mode if nothing was specified. */
    if (!config_explicit(table, "disk.checksum"))
        switch (mmrand(&g.extra_rnd, 1, 10)) {
        case 1:
        case 2:
        case 3:
        case 4: /* 40% */
            config_single(table, "disk.checksum=on", false);
            break;
        case 5: /* 10% */
            config_off(table, "disk.checksum");
            break;
        case 6: /* 10% */
            config_single(table, "disk.checksum=uncompressed", false);
            break;
        default: /* 40% */
            config_single(table, "disk.checksum=unencrypted", false);
            break;
        }
}

/*
 * config_compression --
 *     Compression configuration.
 */
static void
config_compression(TABLE *table, const char *conf_name)
{
    char confbuf[128];
    const char *cstr;

    /* Ignore logging compression if we're not doing logging. */
    if (strcmp(conf_name, "logging.compression") == 0 && GV(LOGGING) == 0) {
        config_off(NULL, "logging.compression");
        return;
    }

    /* Return if already specified and it's a current compression engine. */
    if (config_explicit(table, conf_name)) {
        cstr = "off";
        if (strcmp(conf_name, "logging.compression") == 0)
            cstr = GVS(LOGGING_COMPRESSION);
        else if (strcmp(conf_name, "btree.compression") == 0)
            cstr = TVS(BTREE_COMPRESSION);
        if (memcmp(cstr, "bzip", strlen("bzip")) != 0)
            return;
        WARN("%s: bzip compression no longer supported", conf_name);
    }

    /*
     * Select a compression type from the list of built-in engines. Listed percentages are only
     * correct if all of the possible engines are compiled in.
     */
    cstr = "off";
    switch (mmrand(&g.extra_rnd, 1, 20)) {
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
    case 7:
    case 8:
    case 9: /* 30% snappy */
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

    testutil_snprintf(confbuf, sizeof(confbuf), "%s=%s", conf_name, cstr);
    config_single(table, confbuf, false);
}

/*
 * config_encryption --
 *     Encryption configuration.
 */
static void
config_encryption(void)
{
    /* Encryption: choose something if encryption wasn't specified. */
    if (config_explicit(NULL, "disk.encryption"))
        return;

    /* 70% no encryption, 30% rotn */
    if (mmrand(&g.data_rnd, 1, 10) < 8)
        config_off(NULL, "disk.encryption");
    else
        config_single(NULL, "disk.encryption=rotn-7", false);
}

/*
 * config_fix --
 *     Fixed-length column-store configuration.
 */
static bool
config_fix(TABLE *table)
{
    /*
     * Fixed-length column stores don't support modify operations, and can't be used with
     * predictable replay with insertions.
     */
    return (!config_explicit(table, "ops.pct.modify") &&
      (!GV(RUNS_PREDICTABLE_REPLAY) || !config_explicit(table, "ops.pct.insert")));
}

/*
 * config_var --
 *     Variable-length column-store configuration.
 */
static bool
config_var(TABLE *table)
{
    /*
     * Variable-length column store insertions can't be used with predictable replay.
     */
    return (!GV(RUNS_PREDICTABLE_REPLAY) || !config_explicit(table, "ops.pct.insert"));
}

/*
 * config_in_memory --
 *     Periodically set up an in-memory configuration.
 */
static void
config_in_memory(void)
{
    /*
     * Configure in-memory before anything else, in-memory has many related requirements. Don't
     * configure in-memory if there's any incompatible configurations.
     *
     * Limit the number of tables in any in-memory run, otherwise it's too easy to blow out the
     * cache.
     */
    if (ntables > 10)
        return;
    if (config_explicit(NULL, "background_compact"))
        return;
    if (config_explicit(NULL, "backup"))
        return;
    if (config_explicit(NULL, "block_cache"))
        return;
    if (config_explicit(NULL, "btree.compression"))
        return;
    if (config_explicit(NULL, "checkpoint"))
        return;
    if (config_explicit(NULL, "format.abort"))
        return;
    if (config_explicit(NULL, "import"))
        return;
    if (config_explicit(NULL, "logging"))
        return;
    if (config_explicit(NULL, "ops.alter"))
        return;
    if (config_explicit(NULL, "ops.compaction"))
        return;
    if (config_explicit(NULL, "ops.hs_cursor"))
        return;
    if (config_explicit(NULL, "ops.salvage"))
        return;
    if (config_explicit(NULL, "ops.verify"))
        return;
    if (config_explicit(NULL, "precise_checkpoint"))
        return;
    if (config_explicit(NULL, "runs.mirror"))
        return;
    if (config_explicit(NULL, "runs.predictable_replay"))
        return;

    if (!config_explicit(NULL, "runs.in_memory") && mmrand(&g.extra_rnd, 1, 20) == 1) {
        config_single(NULL, "runs.in_memory=1", false);
        /* Use table[0] to access the global value (RUN_ROWS is a table value). */
        if (NTV(tables[0], RUNS_ROWS) > WT_MILLION) {
            WARN("limiting runs.rows to %d as runs.in_memory has been automatically enabled",
              WT_MILLION);
            config_single(NULL, "runs.rows=" XSTR(WT_MILLION_LITERAL), true);
        }
    }
}

/*
 * config_in_memory_reset --
 *     In-memory configuration review.
 */
static void
config_in_memory_reset(void)
{
    /* If doing an in-memory run, make sure we haven't configured something that won't work. */
    if (!GV(RUNS_IN_MEMORY))
        return;

    /* Turn off a lot of stuff. */
    if (!config_explicit(NULL, "background_compact"))
        config_off(NULL, "background_compact");
    if (!config_explicit(NULL, "backup"))
        config_off(NULL, "backup");
    if (!config_explicit(NULL, "block_cache"))
        config_off(NULL, "block_cache");
    if (!config_explicit(NULL, "checkpoint"))
        config_off(NULL, "checkpoint");
    if (!config_explicit(NULL, "import"))
        config_off(NULL, "import");
    if (!config_explicit(NULL, "logging"))
        config_off(NULL, "logging");
    if (!config_explicit(NULL, "ops.alter"))
        config_off(NULL, "ops.alter");
    if (!config_explicit(NULL, "ops.compaction"))
        config_off(NULL, "ops.compaction");
    if (!config_explicit(NULL, "ops.hs_cursor"))
        config_off(NULL, "ops.hs_cursor");
    if (!config_explicit(NULL, "ops.salvage"))
        config_off(NULL, "ops.salvage");
    if (!config_explicit(NULL, "ops.verify"))
        config_off(NULL, "ops.verify");
    if (!config_explicit(NULL, "precise_checkpoint"))
        config_off(NULL, "precise_checkpoint");
    if (!config_explicit(NULL, "prefetch"))
        config_off(NULL, "prefetch");
}

/*
 * config_mirrors --
 *     Configure table mirroring.
 */
static void
config_mirrors(void)
{
    u_int available_tables, i, mirrors;
    char buf[100];
    bool already_set, explicit_mirror;

    g.mirror_col_store = false;

    /*
     * In theory, mirroring should work with predictable replay, although there's some overlap in
     * functionality. That is, we usually do multiple runs with the same key with predictable replay
     * and would notice if data was different or missing. We disable it to keep runs simple.
     */
    if (GV(RUNS_PREDICTABLE_REPLAY)) {
        WARN("%s", "turning off mirroring for predictable replay");
        config_off_all("runs.mirror");
        return;
    }

    /* Check for a CONFIG file that's already set up for mirroring. */
    for (already_set = false, i = 1; i <= ntables; ++i)
        if (NTV(tables[i], RUNS_MIRROR)) {
            already_set = tables[i]->mirror = true;
            if (tables[i]->type == FIX || tables[i]->type == VAR)
                g.mirror_col_store = true;
            if (g.base_mirror == NULL && tables[i]->type != FIX)
                g.base_mirror = tables[i];
        }
    if (already_set) {
        if (g.base_mirror == NULL)
            testutil_die(EINVAL, "no table configured that can act as the base mirror");

        /* A custom collator would complicate the cursor traversal when comparing tables. */
        config_mirrors_disable_reverse();

        /*
         * Assume that mirroring is already configured if one of the tables has explicitly
         * configured it on. This isn't optimal since there could still be other tables that haven't
         * set it at all (and might be usable as extra mirrors), but that's an uncommon scenario and
         * it lets us avoid a bunch of extra logic around figuring out whether we have an acceptable
         * minimum number of tables.
         */
        return;
    }

    /*
     * Mirror configuration is potentially confusing: it's a per-table configuration (because it has
     * to be set for subsequent runs so we can tell which tables are part of the mirror group), but
     * it's configured on a global basis, causing the random selection of a group of tables for the
     * mirror group. If it's configured anywhere, it's configured everywhere; otherwise configure it
     * 20% of the time. Once that's done, turn off all mirroring, it's turned back on for selected
     * tables.
     */
    explicit_mirror = config_explicit(NULL, "runs.mirror");
    if (!explicit_mirror && mmrand(&g.data_rnd, 1, 10) < 9) {
        config_off_all("runs.mirror");
        return;
    }

    /*
     * We can't mirror if we don't have enough tables. A FLCS table can be a mirror, but it can't be
     * the source of the bulk-load mirror records. Find the first table we can use as a base.
     */
    for (i = 1; i <= ntables; ++i)
        if (tables[i]->type != FIX && !NT_EXPLICIT_OFF(tables[i], RUNS_MIRROR))
            break;

    if (i > ntables) {
        if (explicit_mirror)
            WARN("%s", "table selection didn't support mirroring, turning off mirroring");
        config_off_all("runs.mirror");
        return;
    }

    /*
     * We also can't mirror if we don't have enough tables that have allowed mirroring. It's
     * possible for a table to explicitly set tableX.runs.mirror=0, so check how many tables have
     * done that and remove them from the count of tables we can use for mirroring.
     */
    available_tables = ntables;
    for (i = 1; i <= ntables; ++i)
        if (NT_EXPLICIT_OFF(tables[i], RUNS_MIRROR))
            --available_tables;

    if (available_tables < 2) {
        if (explicit_mirror)
            WARN("%s", "not enough tables left mirroring enabled, turning off mirroring");
        config_off_all("runs.mirror");
        return;
    }

    /* A custom collator would complicate the cursor traversal when comparing tables. */
    config_mirrors_disable_reverse();

    /* Good to go: pick the first non-FLCS table that allows mirroring as our base. */
    for (i = 1; i <= ntables; ++i)
        if (tables[i]->type != FIX && !NT_EXPLICIT_OFF(tables[i], RUNS_MIRROR))
            break;
    tables[i]->mirror = true;
    config_single(tables[i], "runs.mirror=1", false);
    g.base_mirror = tables[i];
    if (tables[i]->type == VAR)
        g.mirror_col_store = true;
    /*
     * Pick some number of tables to mirror, then turn on mirroring the next (n-1) tables, where
     * allowed.
     */
    for (mirrors = mmrand(&g.data_rnd, 2, ntables) - 1, i = 1; i <= ntables; ++i) {
        if (NT_EXPLICIT_OFF(tables[i], RUNS_MIRROR))
            continue;
        if (tables[i] != g.base_mirror) {
            tables[i]->mirror = true;
            config_single(tables[i], "runs.mirror=1", false);
            if (tables[i]->type == FIX || tables[i]->type == VAR)
                g.mirror_col_store = true;
            if (--mirrors == 0)
                break;
        }
    }

    /*
     * Give each mirror the same number of rows (it's not necessary, we could treat end-of-table on
     * a mirror as OK, but this lets us assert matching rows).
     */
    testutil_snprintf(buf, sizeof(buf), "runs.rows=%" PRIu32, NTV(g.base_mirror, RUNS_ROWS));
    for (i = 1; i <= ntables; ++i)
        if (tables[i]->mirror && tables[i] != g.base_mirror)
            config_single(tables[i], buf, false);
}

/*
 * config_mirrors_disable_reverse --
 *     Disable reverse if mirroring enabled.
 */
static void
config_mirrors_disable_reverse(void)
{
    u_int i;

    for (i = 1; i <= ntables; ++i)
        if (NTV(tables[i], BTREE_REVERSE) && config_explicit(tables[i], "btree.reverse")) {
            WARN(
              "%s", "mirroring incompatible with reverse collation, turning off reverse collation");
            break;
        }
    config_off_all("btree.reverse");
}

/*
 * config_pct --
 *     Configure operation percentages.
 */
static void
config_pct(TABLE *table)
{
    struct {
        const char *name; /* Operation */
        uint32_t *vp;     /* Value store */
        u_int order;      /* Order of assignment */
        bool enabled;     /* Enabled for this configuration */
    } list[5];
    u_int i, max_order, max_slot, n, pct;
    bool slot_available;

    /* We explicitly disable modify operations for predictable replay. */
    list[0].name = "ops.pct.delete";
    list[0].vp = &TV(OPS_PCT_DELETE);
    list[0].order = 0;
    list[0].enabled = replay_operation_enabled(REMOVE);
    list[1].name = "ops.pct.insert";
    list[1].vp = &TV(OPS_PCT_INSERT);
    list[1].order = 0;
    list[1].enabled = replay_operation_enabled(INSERT);
    list[2].name = "ops.pct.modify";
    list[2].vp = &TV(OPS_PCT_MODIFY);
    list[2].order = 0;
    list[2].enabled = replay_operation_enabled(MODIFY);
    list[3].name = "ops.pct.read";
    list[3].vp = &TV(OPS_PCT_READ);
    list[3].order = 0;
    list[3].enabled = replay_operation_enabled(READ);
    list[4].name = "ops.pct.write";
    list[4].vp = &TV(OPS_PCT_WRITE);
    list[4].order = 0;
    list[4].enabled = replay_operation_enabled(UPDATE);

    /*
     * Walk the list of operations, checking for an illegal configuration and creating a random
     * order in the list.
     */
    pct = 0;
    slot_available = false;
    for (i = 0; i < WT_ELEMENTS(list); ++i)
        if (list[i].enabled) {
            if (config_explicit(table, list[i].name))
                pct += *list[i].vp;
            else {
                list[i].order = mmrand(&g.data_rnd, 1, WT_THOUSAND);
                slot_available = true;
            }
        }

    /*
     * Some older configurations had broken percentages. If summing the explicitly specified
     * percentages maxes us out, warn and keep running, leaving unspecified operations at 0.
     */
    if (pct > 100 || (pct < 100 && !slot_available)) {
        WARN("operation percentages %s than 100, resetting to random values",
          pct > 100 ? "greater" : "less");
        for (i = 0; i < WT_ELEMENTS(list); ++i)
            list[i].order = mmrand(&g.data_rnd, 1, WT_THOUSAND);
        pct = 0;
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
            if (list[i].order != 0 && list[i].enabled)
                ++n;
            if (list[i].order > max_order && list[i].enabled) {
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
        *list[max_slot].vp = mmrand(&g.data_rnd, 0, pct);
        list[max_slot].order = 0;
        pct -= *list[max_slot].vp;
    }

    testutil_assert(TV(OPS_PCT_DELETE) + TV(OPS_PCT_INSERT) + TV(OPS_PCT_MODIFY) +
        TV(OPS_PCT_READ) + TV(OPS_PCT_WRITE) ==
      100);
}

/*
 * config_run_length --
 *     Run length configuration.
 */
static void
config_run_length(void)
{
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
    if (config_explicit(NULL, "runs.timer")) {
        if (!config_explicit(NULL, "runs.ops"))
            config_single(NULL, "runs.ops=0", false);
    } else {
        if (!config_explicit(NULL, "runs.ops"))
            config_single(NULL, "runs.timer=30", false);
        else
            config_single(NULL, "runs.timer=360", false);
    }

    /*
     * There are combinations that can cause out of disk space issues and here we try to prevent
     * those. CONFIG.stress causes runs.timer to be considered explicit which limits when we can
     * override the run length to extreme cases.
     */
    if (GV(RUNS_TIMER) > 10 && GV(LOGGING) && !GV(LOGGING_REMOVE) && GV(BACKUP) &&
      GV(OPS_SALVAGE)) {
        WARN(
          "limiting runs.timer=%d as logging=1, backup=1, ops.salvage=1, and logging.remove=0", 10);
        config_single(NULL, "runs.timer=10", true);
    }
}

/*
 * config_statistics --
 *     Statistics configuration.
 */
static void
config_statistics(void)
{
    /* Sources is only applicable when the mode is all. */
    if (strcmp(GVS(STATISTICS_MODE), "all") != 0 && strcmp(GVS(STATISTICS_LOG_SOURCES), "off") != 0)
        testutil_die(EINVAL, "statistics sources requires mode to be all");

    if (!config_explicit(NULL, "statistics.mode")) {
        /* 70% of the time set statistics to fast. */
        if (mmrand(&g.extra_rnd, 1, 10) < 8)
            config_single(NULL, "statistics.mode=fast", false);
        else
            config_single(NULL, "statistics.mode=all", false);
    }

    if (!config_explicit(NULL, "statistics_log.sources")) {
        /* 10% of the time use sources if all. */
        if (strcmp(GVS(STATISTICS_MODE), "all") == 0 && mmrand(&g.extra_rnd, 1, 10) == 1)
            config_single(NULL, "statistics_log.sources=file:", false);
    }
}

/*
 * config_chunk_cache --
 *     Chunk cache configuration.
 */
static void
config_chunk_cache(void)
{
    char buf[128];
    const char *chunkcache_type;

    chunkcache_type = NULL;

    /* Chunk cache does not work unless tiered storage is configured. */
    if (!g.tiered_storage_config) {
        if (config_explicit(NULL, "chunk_cache") && GV(CHUNK_CACHE))
            testutil_die(EINVAL,
              "%s: chunk cache cannot be enabled unless tiered storage is configured.", progname);
        return;
    }

    if (!config_explicit(NULL, "chunk_cache")) {
        /*
         * Make sure no configurations related to chunk caching are set if chunk cache is not
         * enabled.
         */
        if (config_explicit(NULL, "chunk_cache.capacity") ||
          config_explicit(NULL, "chunk_cache.chunk_size") ||
          config_explicit(NULL, "chunk_cache.type") ||
          config_explicit(NULL, "chunk_cache.storage_path"))
            testutil_die(EINVAL,
              "%s: Enable chunk caching (chunk_cache=on) to allow configuring other chunk cache "
              "settings",
              progname);

        /* Enable chunk cache 50% of the time if not explicit set. */
        testutil_snprintf(
          buf, sizeof(buf), "chunk_cache=%s", mmrand(&g.data_rnd, 1, 100) <= 50 ? "on" : "off");
        config_single(NULL, buf, false);
    }

    if (GV(CHUNK_CACHE)) {
        if (config_explicit(NULL, "chunk_cache.type")) {
            chunkcache_type = GVS(CHUNK_CACHE_TYPE);
            if (strcmp(chunkcache_type, "FILE") != 0 && strcmp(chunkcache_type, "DRAM") != 0)
                testutil_die(EINVAL, "illegal chunkcache.type configuration: %s", chunkcache_type);

            if (GV(RUNS_IN_MEMORY) && strcmp(chunkcache_type, "FILE") == 0)
                testutil_die(EINVAL,
                  "%s: chunk caching cannot be enabled for in-memory runs as chunkcache.type is "
                  "set to FILE.",
                  progname);
        } else {
            if (GV(RUNS_IN_MEMORY))
                config_single(NULL, "chunk_cache.type=DRAM", false);
            else {
                /*
                 * Alternate between running chunk cache with the 'File' type and the 'DRAM' type.
                 */
                testutil_snprintf(buf, sizeof(buf), "chunk_cache.type=%s",
                  mmrand(&g.data_rnd, 1, 100) <= 50 ? "DRAM" : "FILE");
                config_single(NULL, buf, false);
            }
        }

        if (strcmp(GVS(CHUNK_CACHE_TYPE), "DRAM") == 0 &&
          config_explicit(NULL, "chunk_cache.storage_path"))
            testutil_die(EINVAL,
              "For chunk_cache.type=%s, passing in the chunk_cache.storage_path=%s is unnecessary.",
              chunkcache_type, GVS(CHUNK_CACHE_STORAGE_PATH));

        if (!config_explicit(NULL, "chunk_cache.capacity") &&
          !config_explicit(NULL, "chunk_cache.chunk_size"))
            if (GV(CHUNK_CACHE_CAPACITY) <= GV(CHUNK_CACHE_CHUNK_SIZE))
                GV(CHUNK_CACHE_CHUNK_SIZE) = GV(CHUNK_CACHE_CAPACITY) / 10;

        /* Always ensure that capacity greater than chunk_size. */
        testutil_assert(GV(CHUNK_CACHE_CAPACITY) > GV(CHUNK_CACHE_CHUNK_SIZE));
    }
}

/*
 * config_tiered_storage --
 *     Tiered storage configuration.
 */
static void
config_tiered_storage(void)
{
    const char *storage_source;

    storage_source = GVS(TIERED_STORAGE_STORAGE_SOURCE);

    g.tiered_storage_config =
      (strcmp(storage_source, "off") != 0 && strcmp(storage_source, "none") != 0);
    if (g.tiered_storage_config) {
        /* Tiered storage requires timestamps. */
        config_off(NULL, "transaction.implicit");
        config_single(NULL, "transaction.timestamps=on", true);

        /* If we are flushing, we need a checkpoint thread. */
        if (GV(TIERED_STORAGE_FLUSH_FREQUENCY) > 0)
            config_single(NULL, "checkpoint=on", false);

        /* Salvage and verify are not supported for tiered storage. */
        config_off(NULL, "ops.salvage");
        config_off(NULL, "ops.verify");

        /* Backup is not supported for tiered tables. */
        config_off(NULL, "backup");
        config_off(NULL, "backup.incremental");

        /* Compact is not supported for tiered tables. */
        config_off(NULL, "ops.compaction");
        config_off(NULL, "background_compact");
    } else
        /* Never try flush to tiered storage unless running with tiered storage. */
        config_single(NULL, "tiered_storage.flush_frequency=0", true);
}

/*
 * config_disagg_storage --
 *     Disaggregated storage configuration.
 */
static void
config_disagg_storage(void)
{
    char buf[128];
    const char *mode, *page_log;

    page_log = GVS(DISAGG_PAGE_LOG);

    g.disagg_storage_config = (strcmp(page_log, "off") != 0 && strcmp(page_log, "none") != 0);
    if (g.disagg_storage_config) {
        if (!config_explicit(NULL, "disagg.mode")) {
            /* Randomly assign "leader" or "follower" to disagg.mode with equal probability. */
            testutil_snprintf(buf, sizeof(buf), "disagg.mode=%s",
              mmrand(&g.data_rnd, 1, 100) <= 50 ? "leader" : "follower");
            config_single(NULL, buf, false);
        }

        mode = GVS(DISAGG_MODE);
        if (strcmp(mode, "leader") != 0 && strcmp(mode, "follower") != 0 &&
          strcmp(mode, "switch") != 0)
            testutil_die(EINVAL, "illegal disagg.mode configuration: %s", mode);

        if (strcmp(mode, "switch") == 0)
            /* Randomly assign "leader" or "follower". */
            g.disagg_leader = mmrand(&g.data_rnd, 0, 1);
        else
            g.disagg_leader = strcmp(mode, "leader") == 0;

        /* Disaggregated storage requires timestamps. */
        config_off(NULL, "transaction.implicit");
        config_single(NULL, "transaction.timestamps=on", true);

        /* It makes sense to do checkpoints. */
        if (!config_explicit(NULL, "checkpoint"))
            config_single(NULL, "checkpoint=on", false);

        /* TODO: Some operations are not yet supported for disaggregated storage. */
        config_off(NULL, "ops.salvage");
        config_off(NULL, "backup");
        config_off(NULL, "backup.incremental");
        config_off(NULL, "ops.compaction");
        config_off(NULL, "background_compact");
    }
}

/*
 * config_transaction --
 *     Transaction configuration.
 */
static void
config_transaction(void)
{
    /* Predictable replay requires timestamps. */
    if (GV(RUNS_PREDICTABLE_REPLAY)) {
        config_off(NULL, "transaction.implicit");
        config_single(NULL, "transaction.timestamps=on", true);
    }

    /* Transaction prepare requires timestamps and is incompatible with logging. */
    if (GV(OPS_PREPARE) && config_explicit(NULL, "ops.prepare")) {
        if (!GV(TRANSACTION_TIMESTAMPS) && config_explicit(NULL, "transaction.timestamps"))
            testutil_die(EINVAL, "prepare requires transaction timestamps");
        if (GV(LOGGING) && config_explicit(NULL, "logging"))
            testutil_die(EINVAL, "prepare is incompatible with logging");
    }

    /* Transaction timestamps are incompatible with implicit transactions, logging and salvage. */
    if (GV(TRANSACTION_TIMESTAMPS) && config_explicit(NULL, "transaction.timestamps")) {
        if (GV(TRANSACTION_IMPLICIT) && config_explicit(NULL, "transaction.implicit"))
            testutil_die(
              EINVAL, "transaction.timestamps is incompatible with implicit transactions");
        if (GV(OPS_SALVAGE) && config_explicit(NULL, "ops.salvage")) /* FIXME WT-6431 */
            testutil_die(EINVAL, "transaction.timestamps is incompatible with salvage");
        if (GV(LOGGING) && config_explicit(NULL, "logging"))
            testutil_die(EINVAL, "transaction.timestamps is incompatible with logging");
    }

    /*
     * Incompatible permanent configurations have been checked, now turn off any incompatible flags.
     * Honor the choice if something was set explicitly, next retain a configured prepare (it's not
     * configured often), then match however timestamps are configured.
     */
    if (GV(OPS_PREPARE) && config_explicit(NULL, "ops.prepare")) {
        config_off(NULL, "logging");
        config_single(NULL, "transaction.timestamps=on", false);
        config_off(NULL, "transaction.implicit");
        config_off(NULL, "ops.salvage");
    }
    if (GV(TRANSACTION_TIMESTAMPS) && config_explicit(NULL, "transaction.timestamps")) {
        config_off(NULL, "transaction.implicit");
        config_off(NULL, "ops.salvage");
        config_off(NULL, "logging");
    }
    if (!GV(TRANSACTION_TIMESTAMPS) && config_explicit(NULL, "transaction.timestamps"))
        config_off(NULL, "ops.prepare");
    if (GV(TRANSACTION_IMPLICIT) && config_explicit(NULL, "transaction.implicit")) {
        config_off(NULL, "transaction.timestamps");
        config_off(NULL, "ops.prepare");
    }
    if (GV(LOGGING) && config_explicit(NULL, "logging")) {
        config_off(NULL, "transaction.timestamps");
        config_off(NULL, "ops.prepare");
    }
    if (GV(OPS_SALVAGE) && config_explicit(NULL, "ops.salvage")) { /* FIXME WT-6431 */
        config_off(NULL, "transaction.timestamps");
        config_off(NULL, "ops.prepare");
    }
    if (GV(OPS_PREPARE)) {
        config_off(NULL, "logging");
        config_single(NULL, "transaction.timestamps=on", false);
        config_off(NULL, "transaction.implicit");
        config_off(NULL, "ops.salvage");
    }
    if (GV(TRANSACTION_TIMESTAMPS)) {
        config_off(NULL, "transaction.implicit");
        config_off(NULL, "ops.salvage");
        config_off(NULL, "logging");
    }
    if (!GV(TRANSACTION_TIMESTAMPS)) {
        config_off(NULL, "ops.prepare");
        config_off(NULL, "precise_checkpoint");
        config_off(NULL, "preserve_prepared");
    }
    /* FIXME-WT-15565 Write prepared truncate operation to disk. */
    if (GV(PRECISE_CHECKPOINT) && GV(OPS_PREPARE)) {
        if (config_explicit(NULL, "ops.truncate")) {
            WARN("%s" PRIu32,
              "turning off ops.truncate to work with ops.prepare and precise checkpoint");
        }
        config_off(NULL, "ops.truncate");
    }

    /* Set a default transaction timeout limit if one is not specified. */
    if (!config_explicit(NULL, "transaction.operation_timeout_ms"))
        config_single(NULL, "transaction.operation_timeout_ms=2000", false);

    g.operation_timeout_ms = GV(TRANSACTION_OPERATION_TIMEOUT_MS);
    g.transaction_timestamps_config = GV(TRANSACTION_TIMESTAMPS) != 0;
    g.prepared_id = 1;
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
    for (max_name = 0, cp = configuration_list; cp->name != NULL; ++cp)
        max_name = WT_MAX(max_name, strlen(cp->name));
    for (cp = configuration_list; cp->name != NULL; ++cp)
        fprintf(stderr, "%*s: %s\n", (int)max_name, cp->name, cp->desc);
}

/*
 * config_print_one --
 *     Print out a single configuration setting.
 */
static void
config_print_one(FILE *fp, CONFIG *cp, CONFIGV *v, const char *prefix)
{
    const char *cstr;

    /* Historic versions of format expect "none", instead of "off", for a few configurations. */
    if (F_ISSET(cp, C_STRING)) {
        cstr = v->vstr == NULL ? "off" : v->vstr;
        if (strcmp(cstr, "off") == 0 &&
          (cp->off == V_GLOBAL_DISK_ENCRYPTION || cp->off == V_GLOBAL_LOGGING_COMPRESSION ||
            cp->off == V_TABLE_BTREE_COMPRESSION))
            cstr = "none";
        fprintf(fp, "%s%s=%s\n", prefix, cp->name, cstr);
        return;
    }

    /* Historic versions of format expect log=(archive), not log=(remove). */
    if (g.backward_compatible && cp->off == V_GLOBAL_LOGGING_REMOVE) {
        fprintf(fp, "%slogging.archive=%" PRIu32 "\n", prefix, v->v);
        return;
    }

    fprintf(fp, "%s%s=%" PRIu32 "\n", prefix, cp->name, v->v);
}

/*
 * config_print_table --
 *     Print per-table information.
 */
static void
config_print_table(FILE *fp, TABLE *table)
{
    CONFIG *cp;
    CONFIGV *v, *gv;
    char buf[128];

    testutil_snprintf(buf, sizeof(buf), "table%u.", table->id);
    fprintf(fp, "############################################\n");
    fprintf(fp, "#  TABLE PARAMETERS: table %u\n", table->id);
    fprintf(fp, "############################################\n");

    for (cp = configuration_list; cp->name != NULL; ++cp) {
        /* Skip global items. */
        if (!F_ISSET(cp, C_TABLE))
            continue;
        if (!C_TYPE_MATCH(cp, table->type))
            continue;

        gv = &tables[0]->v[cp->off];
        v = &table->v[cp->off];

        /* Skip entries that match any global setting. */
        if (gv->set && v->v == gv->v &&
          ((v->vstr == NULL && gv->vstr == NULL) ||
            (v->vstr != NULL && gv->vstr != NULL && strcmp(v->vstr, gv->vstr) == 0)))
            continue;

        config_print_one(fp, cp, v, buf);
    }
}

/*
 * config_print --
 *     Print configuration information.
 */
void
config_print(bool error_display)
{
    CONFIG *cp;
    CONFIGV *gv;
    FILE *fp;
    uint32_t i;

    /* Reopening an existing database should leave the existing CONFIG file. */
    if (g.reopen)
        return;

    if (error_display)
        fp = stdout;
    else if ((fp = fopen(g.home_config, "w")) == NULL)
        testutil_die(errno, "fopen: %s", g.home_config);

    fprintf(fp, "############################################\n");
    fprintf(fp, "#  RUN PARAMETERS: V3\n");
    fprintf(fp, "############################################\n");

    /* Display global configuration values. */
    for (cp = configuration_list; cp->name != NULL; ++cp) {
        /* Skip table count if tables not configured (implying an old-style CONFIG file). */
        if (ntables == 0 && cp->off == V_GLOBAL_RUNS_TABLES)
            continue;

        /* Skip mismatched table items if the global table is the only table. */
        if (ntables == 0 && F_ISSET(cp, C_TABLE) && !C_TYPE_MATCH(cp, tables[0]->type))
            continue;

        /* Skip table items if not explicitly set and the global table isn't the only table. */
        gv = &tables[0]->v[cp->off];
        if (ntables > 0 && F_ISSET(cp, C_TABLE) && !gv->set)
            continue;

        /* Print everything else. */
        config_print_one(fp, cp, gv, "");
    }

    /* Display per-table configuration values. */
    if (ntables != 0)
        for (i = 1; i <= ntables; ++i)
            config_print_table(fp, tables[i]);

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

    /*
     * Turn off multi-table configuration for all configuration files, for backward compatibility.
     * This doesn't stop multiple table configurations, using either "runs.tables" or an explicit
     * mention of a table, it only prevents CONFIG files without a table reference from configuring
     * tables. This should only affect putting some non-table-specific configurations into a file
     * and running that file as a CONFIG, expecting a multi-table test, and means old-style CONFIG
     * files don't suddenly turn into multiple table tests.
     */
    g.multi_table_config = false;

    if ((fp = fopen(name, "r")) == NULL)
        testutil_die(errno, "fopen: %s", name);

    /*
     * Skip whitespace leading up to the configuration. Skip Evergreen timestamps by skipping a pair
     * of enclosing braces and trailing whitespace. This is fragile: we're in trouble if Evergreen
     * changes its timestamp format.
     */
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        /* Replace any newline character. */
        for (p = buf; *p != '\0'; ++p)
            if (*p == '\n') {
                *p = '\0';
                break;
            }

        /* Skip any leading whitespace. */
        for (p = buf; *p != '\0'; ++p)
            if (!isblank((unsigned char)*p))
                break;

        /* Skip any Evergreen timestamp. */
        if (*p == '[')
            for (; *p != '\0'; ++p)
                if (*p == ']') {
                    ++p;
                    break;
                }

        /* Skip any trailing whitespace. */
        for (; *p != '\0'; ++p)
            if (!isblank((unsigned char)*p))
                break;

        /* Skip any comments or empty lines. */
        if (*p != '\0' && *p != '#')
            config_single(NULL, p, true);
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
    u_int i, j, slots;

    /* Clear all allocated configuration data in the tables array. */
    slots = ntables == 0 ? 1 : ntables;
    for (i = 0; i < slots; ++i) {
        free(tables[i]->val_base);

        for (j = 0; j < V_ELEMENT_COUNT; ++j)
            free(tables[i]->v[j].vstr);
        free(tables[i]);
    }
}

/*
 * config_find --
 *     Find a specific configuration entry.
 */
static CONFIG *
config_find(const char *s, size_t len, bool fatal)
{
    CONFIG *cp;

    for (cp = configuration_list; cp->name != NULL; ++cp)
        if (strncmp(s, cp->name, len) == 0 && cp->name[len] == '\0')
            return (cp);

    /* Optionally ignore unknown keywords, it makes it easier to run old CONFIG files. */
    if (fatal)
        testutil_die(EINVAL, "%s: %s: unknown required configuration keyword", progname, s);

    WARN("%s: ignoring unknown configuration keyword", s);
    return (NULL);
}

/*
 * config_off --
 *     Turn a configuration value off.
 */
static void
config_off(TABLE *table, const char *s)
{
    CONFIG *cp;
    char buf[100];

    cp = config_find(s, strlen(s), true);
    testutil_snprintf(buf, sizeof(buf), "%s=%s", s, F_ISSET(cp, C_BOOL | C_STRING) ? "off" : "0");
    config_single(table, buf, false);
}

/*
 * config_off_all --
 *     Turn a configuration value off for all possible entries.
 */
static void
config_off_all(const char *s)
{
    u_int i;

    config_off(tables[0], s);
    for (i = 1; i <= ntables; ++i)
        config_off(tables[i], s);
}

/*
 * config_table_extend --
 *     Extend the tables array as necessary.
 */
static void
config_table_extend(u_int ntable)
{
    u_int i;

    if (g.backward_compatible)
        testutil_die(0, "multiple tables not supported in backward compatibility mode");

    if (ntable <= ntables)
        return;

    /*
     * Allocate any new tables structures. (We do it this way, rather than reallocating the whole
     * tables array, because our caller doesn't know we're extending the list of tables, and is
     * likely holding pointers into the current list of tables. Reallocating the whole array would
     * require handling reallocation in our caller, and it's not worth the effort.)
     *
     * This might be the first extension, reset the base table's ID (for debugging, we should never
     * be using a table with ID 0).
     */
    for (i = 0; i <= ntable; ++i) {
        if (tables[i] == NULL)
            tables[i] = dcalloc(1, sizeof(TABLE));
        tables[i]->id = i;
    }
    ntables = ntable;
}

/*
 * config_single --
 *     Set a single configuration structure value.
 */
void
config_single(TABLE *table, const char *s, bool explicit)
{
    WT_RAND_STATE *rnd;
    enum { RANGE_FIXED, RANGE_NONE, RANGE_WEIGHTED } range;
    CONFIG *cp;
    CONFIGV *v;
    uint32_t steps, v1, v2;
    u_long ntable;
    u_int i;
    const u_char *t;
    const char *equalp, *vp1, *vp2;
    char *endptr;

    /*
     * Check for corrupted input. Format has a syntax checking mode and this simplifies that work by
     * checking for any unexpected characters. It's complicated by wiredtiger.config, as that
     * configuration option includes JSON characters.
     */
    for (t = (const u_char *)s; *t != '\0'; ++t)
        if (!__wt_isalnum(*t) && !__wt_isspace(*t) && strchr("\"'()-.:=[]_/,", *t) == NULL)
            testutil_die(
              EINVAL, "%s: configuration contains unexpected character %#x", progname, (u_int)*t);

    /* Skip leading white space. */
    while (__wt_isspace((u_char)*s))
        ++s;

    /*
     * If configuring a single table, the table argument will be non-NULL. The configuration itself
     * may include a table reference, in which case we extend the table as necessary and select the
     * table.
     */
    if (table == NULL) {
        table = tables[0];
        if (strncmp(s, "table", strlen("table")) == 0) {
            errno = 0;
            ntable = strtoul(s + strlen("table"), &endptr, 10);
            testutil_assert(errno == 0 && endptr[0] == '.');
            config_table_extend((uint32_t)ntable);
            table = tables[ntable];

            s = endptr + 1;
        }
    }

    /* Process backward compatibility configuration. */
    config_compat(&s);

    if ((equalp = strchr(s, '=')) == NULL)
        testutil_die(EINVAL, "%s: %s: configuration missing \'=\' character", progname, s);

    /* Find the configuration value, and assert it's not a table/global mismatch. */
    if ((cp = config_find(s, (size_t)(equalp - s), false)) == NULL)
        return;
    testutil_assert(F_ISSET(cp, C_TABLE) || table == tables[0]);

    /* Ignore tables settings in backward compatible runs. */
    if (g.backward_compatible && cp->off == V_GLOBAL_RUNS_TABLES) {
        WARN("backward compatible run, ignoring %s setting", s);
        return;
    }

    ++equalp;
    v = &table->v[cp->off];

    /*
     * Use the data RNG for these options, that's conservative.
     */
    rnd = &g.data_rnd;

    if (F_ISSET(cp, C_STRING)) {
        /*
         * Historically, both "none" and "off" were used for turning off string configurations, now
         * we only use "off".
         */
        if (strcmp(equalp, "none") == 0)
            equalp = "off";

        if (strncmp(s, "backup.incremental", strlen("backup.incremental")) == 0)
            config_map_backup_incr(equalp, &g.backup_incr);
        else if (strncmp(s, "checkpoint", strlen("checkpoint")) == 0)
            config_map_checkpoint(equalp, &g.checkpoint_config);
        else if (strncmp(s, "runs.source", strlen("runs.source")) == 0 &&
          strncmp("file", equalp, strlen("file")) != 0 &&
          strncmp("layered", equalp, strlen("layered")) != 0 &&
          strncmp("table", equalp, strlen("table")) != 0) {
            testutil_die(EINVAL, "Invalid data source option: %s", equalp);
        } else if (strncmp(s, "runs.type", strlen("runs.type")) == 0) {
            /* Save any global configuration for later table configuration. */
            if (table == tables[0])
                testutil_snprintf(g.runs_type, sizeof(g.runs_type), "%s", equalp);

            config_map_file_type(equalp, &table->type);
            equalp = config_file_type(table->type);
        }

        /* Free the previous setting if a configuration has been passed in twice. */
        free(v->vstr);

        v->vstr = dstrdup(equalp);
        v->set = explicit;
        return;
    }

    if (F_ISSET(cp, C_BOOL)) {
        if (strncmp(equalp, "off", strlen("off")) == 0)
            v1 = 0;
        else if (strncmp(equalp, "on", strlen("on")) == 0)
            v1 = 1;
        else {
            v1 = atou32(s, equalp, '\0');
            if (v1 != 0 && v1 != 1)
                testutil_die(EINVAL, "%s: %s: value of boolean not 0 or 1", progname, s);
        }

        v->v = v1;
        v->set = explicit;
        return;
    }

    if (F_ISSET(cp, C_POW2)) {
        v1 = atou32(s, equalp, '\0');
        if (v1 != 0 && !__wt_ispo2(v1))
            testutil_die(EINVAL, "%s: %s: value is not a power of 2", progname, s);

        v->v = v1;
        v->set = explicit;
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

    /*
     * Get the value and check the range; zero is optionally an out-of-band "don't set this
     * variable" value.
     */
    v1 = atou32(s, vp1, range == RANGE_NONE ? '\0' : (range == RANGE_FIXED ? '-' : ':'));
    if (!(v1 == 0 && F_ISSET(cp, C_ZERO_NOTSET)) && (v1 < cp->min || v1 > cp->maxset)) {
        /*
         * Historically, btree.split_pct support ranges < 50; correct the value.
         *
         * Historically, btree.key_min allows ranges under the minimum; correct the value
         */
        if (cp->off == V_TABLE_BTREE_SPLIT_PCT && v1 < 50) {
            v1 = 50;
            WARN("correcting btree.split_pct value to %" PRIu32, v1);
        } else if (cp->off == V_TABLE_BTREE_KEY_MIN && v1 < KEY_LEN_CONFIG_MIN) {
            v1 = KEY_LEN_CONFIG_MIN;
            WARN("correcting btree.key_min value to %" PRIu32, v1);
        } else
            testutil_die(EINVAL, "%s: %s: value outside min/max values of %" PRIu32 "-%" PRIu32,
              progname, s, cp->min, cp->maxset);
    }

    if (range != RANGE_NONE) {
        v2 = atou32(s, vp2, '\0');
        if (v2 < cp->min || v2 > cp->maxset)
            testutil_die(EINVAL, "%s: %s: value outside min/max values of %" PRIu32 "-%" PRIu32,
              progname, s, cp->min, cp->maxset);
        if (v1 > v2)
            testutil_die(EINVAL, "%s: %s: illegal numeric range", progname, s);

        if (range == RANGE_FIXED)
            v1 = mmrand(rnd, (u_int)v1, (u_int)v2);
        else {
            /*
             * Roll dice, 50% chance of proceeding to the next larger value, and 5 steps to the
             * maximum value.
             */
            steps = ((v2 - v1) + 4) / 5;
            if (steps == 0)
                steps = 1;
            for (i = 0; i < 5; ++i, v1 += steps)
                if (mmrand(rnd, 0, 1) == 0)
                    break;
            v1 = WT_MIN(v1, v2);
        }
    }

    v->v = v1;
    v->set = explicit;

    if (strncmp(s, "runs.tables", strlen("runs.tables")) == 0)
        config_table_extend((uint32_t)v1);
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
    v = mmrand(&g.data_rnd, 1, 10);
    if (fix && v == 1)
        *vp = FIX;
    else if (var && (v < 5 || !row))
        *vp = VAR;
    else
        *vp = ROW;
}

/*
 * config_map_backup_incr --
 *     Map an incremental backup configuration to a flag.
 */
static void
config_map_backup_incr(const char *s, bool *vp)
{
    if (strcmp(s, "block") == 0)
        *vp = true;
    else if (strcmp(s, "off") == 0)
        *vp = false;
    /* Compatibility for old configurations. */
    else if (strcmp(s, "log") == 0)
        *vp = false;
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
 * config_explicit --
 *     Return if a configuration entry is explicitly set (as opposed to being randomly set).
 */
static bool
config_explicit(TABLE *table, const char *s)
{
    CONFIG *cp;
    u_int i;

    /* Look up the configuration option. */
    cp = config_find(s, strlen(s), true);

    /*
     * If it's a global option, assert our caller didn't ask for a table value, and return if it's
     * set in the base values.
     */
    if (!F_ISSET(cp, C_TABLE)) {
        testutil_assert(table == NULL);
        return (tables[0]->v[cp->off].set);
    }

    /* If checking a single table, the table argument is non-NULL. */
    if (table != NULL)
        return (table->v[cp->off].set);

    /* Otherwise, check if it's set in the base values or in any table. */
    if (tables[0]->v[cp->off].set)
        return (true);
    for (i = 1; i < ntables; ++i)
        if (tables[i]->v[cp->off].set)
            return (true);
    return (false);
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

/*
 * config_compact --
 *     Generate compaction related configurations.
 */
static void
config_compact(void)
{
    /* Compaction does not work on in-memory databases, disable it. */
    if (GV(RUNS_IN_MEMORY)) {
        if (config_explicit(NULL, "background_compact") && GV(BACKGROUND_COMPACT))
            testutil_die(
              EINVAL, "%s: Background compaction cannot be enabled for in-memory runs", progname);
        if (config_explicit(NULL, "ops.compaction") && GV(OPS_COMPACTION))
            testutil_die(
              EINVAL, "%s: Foreground compaction cannot be enabled for in-memory runs", progname);
        config_off(NULL, "background_compact");
        config_off(NULL, "ops.compaction");
    }
}

/*
 * config_obsolete_cleanup --
 *     Obsolete cleanup configuration.
 */
static void
config_obsolete_cleanup(void)
{
    uint32_t wait_seconds;
    char confbuf[128];

    if (!config_explicit(NULL, "obsolete_cleanup.method")) {
        if (mmrand(&g.extra_rnd, 1, 10) < 2)
            config_single(NULL, "obsolete_cleanup.method=reclaim_space", false);
    }

    if (!config_explicit(NULL, "obsolete_cleanup.wait")) {
        wait_seconds = mmrand(&g.extra_rnd, 1, 3600);
        testutil_snprintf(confbuf, sizeof(confbuf), "obsolete_cleanup.wait=%" PRIu32, wait_seconds);
        config_single(NULL, confbuf, false);
    }
}
