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

GLOBAL g;

static int handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static int handle_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);
static void onint(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void cleanup(bool);
static int usage(void);
static int wt_connect(const char *);
static int wt_shutdown(void);

extern int __wt_optind;
extern char *__wt_optarg;

int
main(int argc, char *argv[])
{
    table_type ttype;
    int ch, cnt, i, ret, runs;
    char *working_dir;
    const char *config_open;
    bool verify_only;

    (void)testutil_set_progname(argv);

    config_open = NULL;
    ret = 0;
    working_dir = NULL;
    ttype = MIX;
    g.checkpoint_name = "WiredTigerCheckpoint";
    g.debug_mode = false;
    g.home = dmalloc(512);
    g.nkeys = 10000;
    g.nops = 100000;
    g.ntables = 3;
    g.nworkers = 1;
    g.sweep_stress = g.use_timestamps = false;
    g.failpoint_hs_delete_key_from_ts = false;
    g.hs_checkpoint_timing_stress = g.reserved_txnid_timing_stress = false;
    g.checkpoint_slow_timing_stress = false;
    g.mixed_mode_deletes = false;
    runs = 1;
    verify_only = false;

    while ((ch = __wt_getopt(progname, argc, argv, "C:c:Dh:k:l:mn:pr:s:T:t:vW:xX")) != EOF)
        switch (ch) {
        case 'c':
            g.checkpoint_name = __wt_optarg;
            break;
        case 'C': /* wiredtiger_open config */
            config_open = __wt_optarg;
            break;
        case 'D':
            g.debug_mode = true;
            break;
        case 'h': /* wiredtiger_open config */
            working_dir = __wt_optarg;
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
            g.mixed_mode_deletes = true;
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
            case '4':
                g.reserved_txnid_timing_stress = true;
                break;
            case '5':
                g.checkpoint_slow_timing_stress = true;
                break;
            default:
                return (usage());
            }
            break;
        case 't':
            switch (__wt_optarg[0]) {
            case 'c':
                ttype = COL;
                break;
            case 'l':
                ttype = LSM;
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
            g.use_timestamps = g.race_timetamps = true;
            break;
        default:
            return (usage());
        }

    argc -= __wt_optind;
    if (argc != 0)
        return (usage());

    /* Clean up on signal. */
    (void)signal(SIGINT, onint);

    testutil_work_dir_from_path(g.home, 512, working_dir);

    /* Start time at 1 since 0 is not a valid timestamp. */
    g.ts_stable = 1;

    printf("%s: process %" PRIu64 "\n", progname, (uint64_t)getpid());
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
            if (ttype == MIX)
                g.cookies[i].type = (table_type)((i % MAX_TABLE_TYPE) + 1);
            else
                g.cookies[i].type = ttype;
            testutil_check(__wt_snprintf(
              g.cookies[i].uri, sizeof(g.cookies[i].uri), "%s%04d", URI_BASE, g.cookies[i].id));
        }

        g.running = 1;

        if ((ret = wt_connect(config_open)) != 0) {
            (void)log_print_err("Connection failed", ret, 1);
            break;
        }

        if (verify_only) {
            WT_SESSION *session;

            if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0) {
                (void)log_print_err("conn.open_session", ret, 1);
                break;
            }

            verify_consistency(session, WT_TS_NONE);
            goto run_complete;
        }

        start_checkpoints();
        if ((ret = start_workers()) != 0) {
            (void)log_print_err("Start workers failed", ret, 1);
            break;
        }

        g.running = 0;
        end_checkpoints();

run_complete:
        free(g.cookies);
        g.cookies = NULL;
        if ((ret = wt_shutdown()) != 0) {
            (void)log_print_err("Shutdown failed", ret, 1);
            break;
        }
    }
    if (g.logfp != NULL)
        (void)fclose(g.logfp);

    /* Ensure that cleanup is done on error. */
    (void)wt_shutdown();
    free(g.cookies);
    return (g.status);
}

#define DEBUG_MODE_CFG ",debug_mode=(eviction=true,table_logging=true),verbose=(recovery)"
/*
 * wt_connect --
 *     Configure the WiredTiger connection.
 */
static int
wt_connect(const char *config_open)
{
    static WT_EVENT_HANDLER event_handler = {
      handle_error, handle_message, NULL, NULL /* Close handler. */
    };
    int ret;
    char config[512];
    char timing_stress_cofing[512];
    bool timing_stress;

    timing_stress = false;

    if (g.sweep_stress || g.failpoint_hs_delete_key_from_ts || g.failpoint_hs_insert_1 ||
      g.failpoint_hs_insert_2 || g.hs_checkpoint_timing_stress || g.reserved_txnid_timing_stress ||
      g.checkpoint_slow_timing_stress) {
        timing_stress = true;
        testutil_check(__wt_snprintf(timing_stress_cofing, sizeof(timing_stress_cofing),
          ",timing_stress_for_test=[%s%s%s%s%s]", g.sweep_stress ? "aggressive_sweep" : "",
          g.failpoint_hs_delete_key_from_ts ? "failpoint_history_store_delete_key_from_ts" : "",
          g.hs_checkpoint_timing_stress ? "history_store_checkpoint_delay" : "",
          g.reserved_txnid_timing_stress ? "checkpoint_reserved_txnid_delay" : "",
          g.checkpoint_slow_timing_stress ? "checkpoint_slow" : ""));
    }

    /*
     * If we want to stress sweep, we have a lot of additional configuration settings to set.
     */
    if (g.sweep_stress)
        testutil_check(__wt_snprintf(config, sizeof(config),
          "create,cache_cursors=false,statistics=(fast),statistics_log=(json,wait=1),error_prefix="
          "\"%s\",file_manager=(close_handle_minimum=1,close_idle_time=1,close_scan_interval=1),"
          "log=(enabled),cache_size=1GB%s%s%s%s",
          progname, timing_stress_cofing, g.debug_mode ? DEBUG_MODE_CFG : "",
          config_open == NULL ? "" : ",", config_open == NULL ? "" : config_open));
    else {
        testutil_check(__wt_snprintf(config, sizeof(config),
          "create,cache_cursors=false,statistics=(fast),statistics_log=(json,wait=1),log=(enabled),"
          "error_prefix=\"%s\"%s%s%s%s",
          progname, g.debug_mode ? DEBUG_MODE_CFG : "", config_open == NULL ? "" : ",",
          config_open == NULL ? "" : config_open, timing_stress ? timing_stress_cofing : ""));
    }
    printf("WT open config: %s\n", config);
    if ((ret = wiredtiger_open(g.home, &event_handler, config, &g.conn)) != 0)
        return (log_print_err("wiredtiger_open", ret, 1));
    return (0);
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

    printf("Closing connection\n");
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
    g.running = 0;
    g.ntables_created = 0;
    g.ts_oldest = 0;

    if (remove_dir)
        testutil_make_work_dir(g.home);
}

static int
handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *errmsg)
{
    WT_UNUSED(handler);
    WT_UNUSED(session);
    WT_UNUSED(error);

    return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    WT_UNUSED(handler);
    WT_UNUSED(session);

    if (g.logfp != NULL)
        return (fprintf(g.logfp, "%s\n", message) < 0 ? -1 : 0);

    return (printf("%s\n", message) < 0 ? -1 : 0);
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
 * log_print_err --
 *     Report an error and return the error.
 */
int
log_print_err(const char *m, int e, int fatal)
{
    if (fatal) {
        g.running = 0;
        g.status = e;
    }
    fprintf(stderr, "%s: %s: %s\n", progname, m, wiredtiger_strerror(e));
    if (g.logfp != NULL)
        fprintf(g.logfp, "%s: %s: %s\n", progname, m, wiredtiger_strerror(e));
    return (e);
}

/*
 * path_setup --
 *     Build the standard paths and shell commands we use.
 */
const char *
type_to_string(table_type type)
{
    if (type == COL)
        return ("COL");
    if (type == LSM)
        return ("LSM");
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
      "usage: %s [-C wiredtiger-config] [-c checkpoint] [-h home] [-k keys]\n\t[-l log] [-m] "
      "[-n ops] [-r runs] [-s 1|2|3|4] [-T table-config] [-t f|r|v]\n\t[-W workers]\n",
      progname);
    fprintf(stderr, "%s",
      "\t-C specify wiredtiger_open configuration arguments\n"
      "\t-c checkpoint name to used named checkpoints\n"
      "\t-h set a database home directory\n"
      "\t-k set number of keys to load\n"
      "\t-l specify a log file\n"
      "\t-m run with mixed mode delete operations\n"
      "\t-n set number of operations each thread does\n"
      "\t-p use prepare\n"
      "\t-r set number of runs (0 for continuous)\n"
      "\t-s specify which timing stress configuration to use ( 1 | 2 | 3 | 4 | 5 )\n"
      "\t\t1: sweep_stress\n"
      "\t\t2: failpoint_hs_delete_key_from_ts\n"
      "\t\t3: hs_checkpoint_timing_stress\n"
      "\t\t4: reserved_txnid_timing_stress\n"
      "\t\t5: checkpoint_slow_timing_stress\n"
      "\t-T specify a table configuration\n"
      "\t-t set a file type ( col | mix | row | lsm )\n"
      "\t-v verify only\n"
      "\t-W set number of worker threads\n"
      "\t-x use timestamps\n"
      "\t-X race timestamp updates with checkpoints\n");
    return (EXIT_FAILURE);
}
