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

#include "test_checkpoint.h"

#define SHARED_PARSE_OPTIONS "b:P:h:"

GLOBAL g;

static int handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static int handle_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);
static void onint(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static int enable_disagg(const char *);
static void cleanup(bool);
static int usage(void);
static void wt_connect(const char *);
static int wt_shutdown(void);

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * init_thread_data --
 *     Initialize the thread data struct.
 */
static void
init_thread_data(THREAD_DATA *td, int info)
{
    td->info = info;
    /*
     * For a predictable replay have a non-overlapping key space for each thread. Also divide the
     * key range between the threads. Otherwise, share the key space among all the threads.
     */
    if (g.predictable_replay) {
        td->start_key = (u_int)info * WT_MILLION + 1;
        td->key_range = g.nkeys / (u_int)g.nworkers;
    } else {
        td->start_key = 1;
        td->key_range = g.nkeys;
    }

    /*
     * For a predictable replay the worker threads use a predetermined set of timestamps. They
     * publish their most recently used timestamps for the clock thread to read across the workers
     * to base their decision on.
     */
    td->ts = 0;
    testutil_random_from_random(&td->data_rnd, &g.opts.data_rnd);
    testutil_random_from_random(&td->extra_rnd, &g.opts.extra_rnd);
}

/*
 * main --
 *     Main function for the test program. See usage() for command line options.
 */
int
main(int argc, char *argv[])
{
    table_type ttype;
    int base, ch, cnt, i, ret, runs;
    char config_open[1024];
    char *end_number, *stop_arg;
    bool verify_only;

    (void)testutil_set_progname(argv);

    memset(config_open, 0, sizeof(config_open));
    ret = 0;
    ttype = MIX;
    g.checkpoint_name = "WiredTigerCheckpoint";
    g.debug_mode = false;
    g.home = dmalloc(512);
    g.nkeys = 10 * WT_THOUSAND;
    g.nops = 100 * WT_THOUSAND;
    g.stop_ts = 0;
    g.ntables = 3;
    g.nworkers = 1;
    g.evict_reposition_timing_stress = false;
    g.sweep_stress = g.use_timestamps = false;
    g.failpoint_eviction_split = false;
    g.failpoint_hs_delete_key_from_ts = false;
    g.failpoint_rec_before_wrapup = false;
    g.hs_checkpoint_timing_stress = false;
    g.checkpoint_slow_timing_stress = false;
    g.no_ts_deletes = false;
    g.predictable_replay = false;
    runs = 1;
    verify_only = false;

    testutil_parse_begin_opt(argc, argv, SHARED_PARSE_OPTIONS, &g.opts);

    while ((ch = __wt_getopt(
              progname, argc, argv, "C:c:d:Dk:l:mn:pr:Rs:S:T:t:vW:xX" SHARED_PARSE_OPTIONS)) != EOF)
        switch (ch) {
        case 'c':
            g.checkpoint_name = __wt_optarg;
            break;
        case 'C': /* wiredtiger_open config */
            strncpy(config_open, __wt_optarg, sizeof(config_open) - 1);
            break;
        case 'd': /* disaggregated storage options */
            if (enable_disagg(__wt_optarg) != 0) {
                return (usage());
            }
            break;
        case 'D':
            g.debug_mode = true;
            break;
        case 'k': /* rows */
            g.nkeys = (u_int)atoi(__wt_optarg);
            break;
        case 'l': /* log */
            if ((g.logfp = fopen(__wt_optarg, "w")) == NULL) {
                fprintf(stderr, "%s: %s\n", __wt_optarg, strerror(errno));
                return (EXIT_FAILURE);
            }
            break;
        case 'm':
            g.no_ts_deletes = true;
            break;
        case 'n': /* operations */
            g.nops = (u_int)atoi(__wt_optarg);
            break;
        case 'p': /* prepare */
            g.prepare = true;
            break;
        case 'r': /* runs */
            runs = atoi(__wt_optarg);
            break;
        case 'R': /* predictable replay */
            g.predictable_replay = true;
            break;
        case 's':
            switch (__wt_optarg[0]) {
            case '1':
                g.sweep_stress = true;
                break;
            case '2':
                g.failpoint_hs_delete_key_from_ts = true;
                break;
            case '3':
                g.hs_checkpoint_timing_stress = true;
                break;
            case '5':
                g.checkpoint_slow_timing_stress = true;
                break;
            case '6':
                g.evict_reposition_timing_stress = true;
                break;
            case '7':
                g.failpoint_eviction_split = true;
                break;
            case '8':
                g.failpoint_rec_before_wrapup = true;
                break;
            default:
                return (usage());
            }
            break;
        case 'S': /* run until this stable timestamp */
            stop_arg = __wt_optarg;
            if (WT_PREFIX_MATCH(stop_arg, "0x")) {
                base = 16;
                stop_arg += 2;
            } else
                base = 10;
            g.stop_ts = strtoull(stop_arg, &end_number, base);
            if (*end_number)
                return (usage());
            break;
        case 't':
            switch (__wt_optarg[0]) {
            case 'c':
                ttype = COL;
                break;
            case 'f':
                ttype = FIX;
                break;
            case 'm':
                ttype = MIX;
                break;
            case 'r':
                ttype = ROW;
                break;
            default:
                return (usage());
            }
            break;
        case 'T':
            g.ntables = atoi(__wt_optarg);
            break;
        case 'v':
            verify_only = true;
            break;
        case 'W':
            g.nworkers = atoi(__wt_optarg);
            break;
        case 'x':
            g.use_timestamps = true;
            break;
        case 'X':
            g.use_timestamps = g.race_timestamps = true;
            break;
        default:
            /* The option is either one that we're asking testutil to support, or illegal. */
            if (testutil_parse_single_opt(&g.opts, ch) != 0)
                return (usage());
        }

    argc -= __wt_optind;
    if (argc != 0)
        return (usage());

    if (g.stop_ts > 0 && (!g.predictable_replay || !g.use_timestamps)) {
        fprintf(stderr, "-S is only valid if specified along with -X and -R.\n");
        return (EXIT_FAILURE);
    }

    if (g.opts.disagg_storage) {
        if (!g.use_timestamps) {
            fprintf(stderr, "disaggregated storage feature requires usage of timestamps (-x/-X)");
            return (EXIT_FAILURE);
        }
        if (ttype != ROW) {
            fprintf(
              stderr, "disaggregated storage feature only supports row store table types (-r)");
            return (EXIT_FAILURE);
        }
        if (strcmp(g.checkpoint_name, "WiredTigerCheckpoint") != 0) {
            fprintf(
              stderr, "disaggregated storage feature doesn't supports named checkpoints (-c)");
            return (EXIT_FAILURE);
        }
        /* FIXME-WT-15051 Disagg is not support prepared operations yet. */
        if (g.prepare == true) {
            fprintf(
              stderr, "disaggregated storage feature doesn't supports prepare operations (-p)");
            return (EXIT_FAILURE);
        }
    }

    /*
     * Among other things, this initializes the random number generators in the option structure.
     */
    testutil_parse_end_opt(&g.opts);
    /* Clean up on signal. */
    (void)signal(SIGINT, onint);

    testutil_work_dir_from_path(g.home, 512, (&g.opts)->home);

    /*
     * Always preserve home directory. Some tests rely on the home directory being present to
     * compare results between runs.
     */
    g.opts.preserve = true;

    /* Start time at 1 since 0 is not a valid timestamp. */
    g.ts_stable = 1;
    g.ts_oldest = 1;
    g.prepared_id = 1;

    printf("%s: process %" PRIu64 "\n", progname, (uint64_t)getpid());
    if (g.predictable_replay)
        printf("Config to seed for replay: " TESTUTIL_SEED_FORMAT "\n", g.opts.data_seed,
          g.opts.extra_seed);

    for (cnt = 1; (runs == 0 || cnt <= runs) && g.status == 0; ++cnt) {
        cleanup(cnt == 1 && !verify_only); /* Clean up previous runs */

        printf("    %d: %d workers, %d tables\n", cnt, g.nworkers, g.ntables);

        /* Setup a fresh set of cookies in the global array. */
        if ((g.cookies = calloc((size_t)(g.ntables), sizeof(COOKIE))) == NULL) {
            (void)log_print_err("No memory", ENOMEM, 1);
            break;
        }

        for (i = 0; i < g.ntables; ++i) {
            g.cookies[i].id = i;
            if (ttype == MIX) {
                g.cookies[i].type = (table_type)((i % MAX_TABLE_TYPE) + 1);
            } else
                g.cookies[i].type = ttype;
            testutil_snprintf(
              g.cookies[i].uri, sizeof(g.cookies[i].uri), "%s%04d", URI_BASE, g.cookies[i].id);
        }

        /*
         * Setup thread data. There are N worker threads, a checkpoint thread and possibly a clock
         * thread. The workers have ID 0 to N-1, checkpoint thread has N, and the clock thread has N
         * + 1.
         */
        if ((g.td = calloc((size_t)(g.nworkers + 2), sizeof(THREAD_DATA))) == NULL) {
            (void)log_print_err("No memory", ENOMEM, 1);
            break;
        }
        for (i = 0; i < g.nworkers; ++i)
            init_thread_data(&g.td[i], i);
        init_thread_data(&g.td[g.nworkers], g.nworkers); /* Checkpoint thread. */
        if (g.use_timestamps)
            init_thread_data(&g.td[g.nworkers + 1], g.nworkers + 1); /* Clock thread. */

        g.opts.running = true;

        wt_connect(config_open);

        if (verify_only) {
            WT_SESSION *session;

            if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0) {
                (void)log_print_err("conn.open_session", ret, 1);
                break;
            }

            verify_consistency(session, WT_TS_NONE, false);
            goto run_complete;
        }

        start_threads();
        ret = start_workers();
        g.opts.running = false;
        end_threads();
        if (ret != 0) {
            (void)log_print_err("Start workers failed", ret, 1);
            break;
        }

run_complete:
        free(g.cookies);
        g.cookies = NULL;
        free(g.td);
        g.td = NULL;
        if ((ret = wt_shutdown()) != 0) {
            (void)log_print_err("Shutdown failed", ret, 1);
            break;
        }
        g.opts.conn = NULL;
    }
    if (g.logfp != NULL)
        (void)fclose(g.logfp);

    /* Ensure that cleanup is done on error. */
    (void)wt_shutdown();
    free(g.cookies);
    testutil_cleanup(&g.opts);

    return (g.status);
}

/*
 * enable_disagg --
 *     Enable disaggregated storage with given mode.
 */
static int
enable_disagg(const char *mode)
{
    if (strcmp(mode, "leader") == 0) {
        g.opts.disagg_storage = true;
        g.opts.disagg_switch_mode = false;
        g.opts.disagg_mode = "leader";
        g.opts.disagg_page_log = "palm";
    } else if (strcmp(mode, "follower") == 0) {
        g.opts.disagg_storage = true;
        g.opts.disagg_mode = "follower";
        g.opts.disagg_switch_mode = false;
        g.opts.disagg_page_log = "palm";
    } else if (strcmp(mode, "switch") == 0) {
        g.opts.disagg_storage = true;
        g.opts.disagg_switch_mode = true;
        /* For switch mode, randomly pick initial role */
        bool disagg_leader = (__wt_random(&g.opts.extra_rnd) % 2) == 0;
        g.opts.disagg_mode = disagg_leader ? "leader" : "follower";
        g.opts.disagg_page_log = "palm";
        printf("Switch mode: starting as %s\n", g.opts.disagg_mode);
    } else {
        fprintf(stderr, "Invalid disaggregated mode: %s\n", mode);
        return EINVAL;
    }

    g.opts.palm_map_size_mb = 2048; /* Set 2GB map size for palm by default. */

    return 0;
}

#define DEBUG_MODE_CFG ",debug_mode=(eviction=true,table_logging=true),verbose=(recovery)"
#define SWEEP_CFG ",file_manager=(close_handle_minimum=1,close_idle_time=1,close_scan_interval=1)"

/*
 * wt_connect --
 *     Configure the WiredTiger connection.
 */
static void
wt_connect(const char *config_open)
{
    static WT_EVENT_HANDLER event_handler = {handle_error, handle_message, NULL, NULL, NULL};
    char buf[512], config[1024];
    bool fast_eviction;

    fast_eviction = false;

    /*
     * Randomly decide on the eviction rate (fast or default).
     */
    if ((__wt_random(&g.opts.extra_rnd) % 15) % 2 == 0)
        fast_eviction = true;

    /* Set up the basic configuration string first. */
    testutil_snprintf(config, sizeof(config),
      "create,cache_cursors=false,statistics=(all),statistics_log=(json,on_close,wait=1),log=("
      "enabled),"
      "error_prefix=\"%s\",cache_size=1G, eviction_dirty_trigger=%i, "
      "eviction_dirty_target=%i,%s%s%s",
      progname, fast_eviction ? 5 : 20, fast_eviction ? 1 : 5, g.debug_mode ? DEBUG_MODE_CFG : "",
      config_open == NULL ? "" : ",", config_open == NULL ? "" : config_open);

    if (g.evict_reposition_timing_stress || g.sweep_stress || g.failpoint_eviction_split ||
      g.failpoint_hs_delete_key_from_ts || g.failpoint_rec_before_wrapup ||
      g.hs_checkpoint_timing_stress || g.checkpoint_slow_timing_stress) {
        testutil_snprintf(buf, sizeof(buf), ",timing_stress_for_test=[%s%s%s%s%s%s%s]",
          g.checkpoint_slow_timing_stress ? "checkpoint_slow" : "",
          g.evict_reposition_timing_stress ? "evict_reposition" : "",
          g.failpoint_eviction_split ? "failpoint_eviction_split" : "",
          g.failpoint_hs_delete_key_from_ts ? "failpoint_history_store_delete_key_from_ts" : "",
          g.failpoint_rec_before_wrapup ? "failpoint_rec_before_wrapup" : "",
          g.hs_checkpoint_timing_stress ? "history_store_checkpoint_delay" : "",
          g.sweep_stress ? "aggressive_sweep" : "");
        strcat(config, buf);
    }

    /*
     * If we want to stress sweep, we have a lot of additional configuration settings to set.
     */
    if (g.sweep_stress)
        strcat(config, SWEEP_CFG);

    /*
     * If we are using tiered add in the extension and tiered storage configuration.
     */
    if (g.opts.tiered_storage) {
        testutil_snprintf(buf, sizeof(buf), "%s/bucket", g.home);
        testutil_recreate_dir(buf);
    }

    printf("WT open config: %s\n", config);
    fflush(stdout);
    testutil_wiredtiger_open(&g.opts, g.home, config, &event_handler, &g.conn, false, false);

    if (g.opts.tiered_storage) {
        /* testutil_tiered_begin needs the connection. */
        g.opts.conn = g.conn;

        /* Set up a random delay for the first flush. */
        set_flush_tier_delay(&g.opts.extra_rnd);
        testutil_tiered_begin(&g.opts);
    }
}

/*
 * wt_shutdown --
 *     Shut down the WiredTiger connection.
 */
static int
wt_shutdown(void)
{
    int ret;

    if (g.conn == NULL)
        return (0);

    if (g.opts.tiered_storage)
        testutil_tiered_end(&g.opts);

    printf("Closing connection\n");
    fflush(stdout);
    ret = g.conn->close(g.conn, NULL);
    g.conn = NULL;
    if (ret != 0)
        return (log_print_err("conn.close", ret, 1));
    return (0);
}

/*
 * cleanup --
 *     Clean up from previous runs.
 */
static void
cleanup(bool remove_dir)
{
    g.opts.running = false;
    g.ntables_created = 0;

    if (remove_dir)
        testutil_recreate_dir(g.home);
}

/*
 * handle_error --
 *     TODO: Add a comment describing this function.
 */
static int
handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *errmsg)
{
    int ret;

    WT_UNUSED(handler);
    WT_UNUSED(session);
    WT_UNUSED(error);

    ret = fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0;
    fflush(stderr);
    return (ret);
}

/*
 * handle_message --
 *     TODO: Add a comment describing this function.
 */
static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    int ret;

    WT_UNUSED(handler);
    WT_UNUSED(session);

    if (g.logfp != NULL)
        return (fprintf(g.logfp, "%s\n", message) < 0 ? -1 : 0);

    ret = printf("%s\n", message) < 0 ? -1 : 0;
    fflush(stdout);
    return (ret);
}

/*
 * onint --
 *     Interrupt signal handler.
 */
static void
onint(int signo)
{
    WT_UNUSED(signo);

    cleanup(false);

    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

/*
 * log_print_err_worker --
 *     Report an error and return the error.
 */
int
log_print_err_worker(const char *func, int line, const char *m, int e, int fatal)
{
    if (fatal) {
        g.opts.running = false;
        g.status = e;
    }
    fprintf(stderr, "%s: %s,%d: %s: %s\n", progname, func, line, m, wiredtiger_strerror(e));
    fflush(stderr);
    if (g.logfp != NULL)
        fprintf(g.logfp, "%s: %s,%d: %s: %s\n", progname, func, line, m, wiredtiger_strerror(e));
    return (e);
}

/*
 * Value encoding for FLCS tables.
 *
 * The string value is a large number of digits pushed around arbitrarily with modify. This is
 * difficult to track incrementally in any useful way with just 8 bits. We try to track the offset
 * of the first digit that's a prime (2, 3, 5, or 7), and which prime it is. We encode this as
 * digit-number * 4 + [2 -> 0; 3 -> 1; 5 -> 2; 7 -> 3], plus 1 overall so as to never store zero.
 * (That allows assuming any zero read back is a deleted value.) If there is no such digit, we
 * return FLCS_NONE. If we lose track, we return FLCS_UNKNOWN. This allows remembering offsets up to
 * 62 before we lose track.
 */

#define FLCS_OFFSET 1 /* avoid storing zero */

/* The magic values are to be tested _before_ subtracting off FLCS_OFFSET. */
#define FLCS_NONE 254
/* FLCS_UNKNOWN lives in test_checkpoint.h so it can be used in compare_cursors(). */

#define FLCS_TRACKED_DIGIT(c) ((c) == '2' || (c) == '3' || (c) == '5' || (c) == '7')

/*
 * flcs_encode_value --
 *     Store an offset and digit in an 8-bit value.
 */
static uint8_t
flcs_encode_value(size_t offset, char digit)
{
    uint8_t digitx;

    if (offset > 62)
        return FLCS_UNKNOWN;

    if (digit == '2')
        digitx = 0;
    else if (digit == '3')
        digitx = 1;
    else if (digit == '5')
        digitx = 2;
    else
        digitx = 3;

    return (FLCS_OFFSET + (uint8_t)(offset * 4 + digitx));
}

/*
 * flcs_decode_value --
 *     Unpack flcs_encode_value results.
 */
static void
flcs_decode_value(uint8_t value, size_t *offsetp, char *digitp)
{
    static const char digits[] = "2357";

    value -= FLCS_OFFSET;

    *offsetp = value >> 2;
    *digitp = digits[value & 3];
}

/*
 * flcs_encode --
 *     Extract the corresponding 8-bit FLCS value from a string value.
 */
uint8_t
flcs_encode(const char *s)
{
    u_int i;

    for (i = 0; s[i] != '\0'; i++) {
        if (FLCS_TRACKED_DIGIT(s[i]))
            return (flcs_encode_value(i, s[i]));
    }
    return (FLCS_NONE);
}

/*
 * flcs_modify --
 *     Update the corresponding 8-bit FLCS value given a modify applied to its string.
 */
uint8_t
flcs_modify(WT_MODIFY *entries, int nentries, uint8_t oldval)
{
    size_t j, offset;
    int i;
    char digit, newdigit;

    newdigit = 0; /* clang -Wconditional-uninitialized */

    /* If we've lost track, we've lost track. */
    if (oldval == FLCS_UNKNOWN)
        return (FLCS_UNKNOWN);

    if (oldval == FLCS_NONE) {
        offset = 0;
        digit = '\0';
    } else
        flcs_decode_value(oldval, &offset, &digit);

    for (i = 0; i < nentries; i++) {
        /* If it starts after us, never mind. */
        if (digit != 0 && entries[i].offset > offset)
            continue;
        /* Find the first appropriate digit. */
        for (j = 0; j < entries[i].data.size; j++) {
            newdigit = ((const char *)entries[i].data.data)[j];
            if (FLCS_TRACKED_DIGIT(newdigit))
                break;
        }
        if (j < entries[i].data.size) {
            /* Found a suitable digit. Remember it. */
            offset = entries[i].offset + j;
            digit = newdigit;
            continue;
        }

        /* If at this point we had no position before, we still don't. */
        if (digit == 0)
            continue;

        /* If this modify overwrote us, we lost track. */
        if (entries[i].offset + entries[i].size > offset)
            return (FLCS_UNKNOWN);

        /* Otherwise, it is fully in front of us, so update our offset and keep going. */
        offset = offset - entries[i].size + entries[i].data.size;
    }

    return (digit == 0 ? FLCS_NONE : flcs_encode_value(offset, digit));
}

/*
 * disagg_switch_roles --
 *     Toggle the current disagg role between "leader" and "follower".
 */
int
disagg_switch_roles(void)
{
    testutil_assert(g.opts.disagg_storage);
    testutil_assert(g.opts.disagg_switch_mode);

    const char *disagg_role = strcmp(g.opts.disagg_mode, "leader") == 0 ? "follower" : "leader";
    char disagg_cfg[64];
    testutil_snprintf(disagg_cfg, sizeof(disagg_cfg), "disaggregated=(role=\"%s\")", disagg_role);

    return (g.conn->reconfigure(g.conn, disagg_cfg));
}

/*
 * type_to_string --
 *     Return the string name of a table type.
 */
const char *
type_to_string(table_type type)
{
    if (type == COL)
        return ("COL");
    if (type == FIX)
        return ("FIX");
    if (type == ROW)
        return ("ROW");
    if (type == MIX)
        return ("MIX");
    return ("INVALID");
}

/*
 * usage --
 *     Display usage statement and exit failure.
 */
static int
usage(void)
{
    fprintf(stderr,
      "usage: %s\n"
      "    [-DmpRvXx] [-C wiredtiger-config] [-c checkpoint] [-d disagg-mode] [-h home] [-k keys] "
      "[-l log]\n"
      "    [-n ops] [-r runs] [-s 1|2|3|4|5] [-T table-config] [-t f|r|v]\n"
      "    [-W workers]\n",
      progname);
    fprintf(stderr, "%s",
      "\t-C specify wiredtiger_open configuration arguments\n"
      "\t-c checkpoint name to used named checkpoints\n"
      "\t-d disaggregated storage mode (leader | follower | switch)\n"
      "\t-D debug mode\n"
      "\t-h set a database home directory\n"
      "\t-k set number of keys to load\n"
      "\t-l specify a log file\n"
      "\t-m perform delete operations without timestamps\n"
      "\t-n set number of operations each thread does\n"
      "\t-p use prepare\n"
      "\t-r set number of runs (0 for continuous)\n"
      "\t-R configure predictable replay\n"
      "\t-s specify which timing stress configuration to use ( 1 | 2 | 3 | 4 | 5 )\n"
      "\t-S set a stable timestamp to stop the test run\n"
      "\t\t1: sweep_stress\n"
      "\t\t2: failpoint_hs_delete_key_from_ts\n"
      "\t\t3: hs_checkpoint_timing_stress\n"
      "\t\t5: checkpoint_slow_timing_stress\n"
      "\t\t6: evict_reposition_timing_stress\n"
      "\t\t7: failpoint_eviction_split\n"
      "\t\t8: failpoint_rec_before_wrapup\n"
      "\t-T specify a table configuration\n"
      "\t-t set a file type ( col | mix | row )\n"
      "\t-v verify only\n"
      "\t-W set number of worker threads\n"
      "\t-X race timestamp updates with checkpoints\n"
      "\t-x use timestamps\n");
    return (EXIT_FAILURE);
}
