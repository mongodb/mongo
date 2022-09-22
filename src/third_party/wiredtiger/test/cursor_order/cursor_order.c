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

#include "cursor_order.h"

static char home[512]; /* Program working dir */
static FILE *logfp;    /* Log file */

static int handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static int handle_message(WT_EVENT_HANDLER *, WT_SESSION *, const char *);
static void onint(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void shutdown(void);
static int usage(void);
static void wt_connect(SHARED_CONFIG *, char *);
static void wt_shutdown(SHARED_CONFIG *);

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    SHARED_CONFIG _cfg, *cfg;
    int ch, cnt, runs;
    char *config_open, *working_dir;

    (void)testutil_set_progname(argv);

    cfg = &_cfg;
    config_open = NULL;
    working_dir = NULL;
    runs = 1;

    /*
     * Explicitly initialize the shared configuration object before parsing command line options.
     */
    cfg->append_inserters = 1;
    cfg->conn = NULL;
    cfg->ftype = ROW;
    cfg->max_nops = 1000000;
    cfg->multiple_files = false;
    cfg->nkeys = 1000;
    cfg->reverse_scanners = 5;
    cfg->reverse_scan_ops = 10;
    cfg->thread_finish = false;
    cfg->vary_nops = false;

    while ((ch = __wt_getopt(progname, argc, argv, "C:Fk:h:l:n:R:r:t:vw:W:")) != EOF)
        switch (ch) {
        case 'C': /* wiredtiger_open config */
            config_open = __wt_optarg;
            break;
        case 'F': /* multiple files */
            cfg->multiple_files = true;
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'k': /* rows */
            cfg->nkeys = (uint64_t)atol(__wt_optarg);
            break;
        case 'l': /* log */
            if ((logfp = fopen(__wt_optarg, "w")) == NULL) {
                fprintf(stderr, "%s: %s\n", __wt_optarg, strerror(errno));
                return (EXIT_FAILURE);
            }
            break;
        case 'n': /* operations */
            cfg->max_nops = (uint64_t)atol(__wt_optarg);
            break;
        case 'R':
            cfg->reverse_scanners = (uint64_t)atol(__wt_optarg);
            break;
        case 'r': /* runs */
            runs = atoi(__wt_optarg);
            break;
        case 't':
            switch (__wt_optarg[0]) {
            case 'f':
                cfg->ftype = FIX;
                break;
            case 'r':
                cfg->ftype = ROW;
                break;
            case 'v':
                cfg->ftype = VAR;
                break;
            default:
                return (usage());
            }
            break;
        case 'v': /* vary operation count */
            cfg->vary_nops = true;
            break;
        case 'w':
            cfg->reverse_scan_ops = (uint64_t)atol(__wt_optarg);
            break;
        case 'W':
            cfg->append_inserters = (uint64_t)atol(__wt_optarg);
            break;
        default:
            return (usage());
        }

    argc -= __wt_optind;
    if (argc != 0)
        return (usage());

    testutil_work_dir_from_path(home, 512, working_dir);

    if (cfg->vary_nops && !cfg->multiple_files) {
        fprintf(stderr, "Variable op counts only supported with multiple tables\n");
        return (usage());
    }

    /* Clean up on signal. */
    (void)signal(SIGINT, onint);

    printf("%s: process %" PRIu64 "\n", progname, (uint64_t)getpid());
    for (cnt = 1; runs == 0 || cnt <= runs; ++cnt) {
        printf("    %d: %" PRIu64 " reverse scanners, %" PRIu64 " writers\n", cnt,
          cfg->reverse_scanners, cfg->append_inserters);

        shutdown(); /* Clean up previous runs */

        wt_connect(cfg, config_open); /* WiredTiger connection */

        ops_start(cfg);

        wt_shutdown(cfg); /* WiredTiger shut down */
    }
    return (0);
}

/*
 * wt_connect --
 *     Configure the WiredTiger connection.
 */
static void
wt_connect(SHARED_CONFIG *cfg, char *config_open)
{
    static WT_EVENT_HANDLER event_handler = {
      handle_error, handle_message, NULL, NULL /* Close handler. */
    };
    char config[512];

    testutil_clean_work_dir(home);
    testutil_make_work_dir(home);

    testutil_check(
      __wt_snprintf(config, sizeof(config), "create,statistics=(all),error_prefix=\"%s\",%s%s",
        progname, config_open == NULL ? "" : ",", config_open == NULL ? "" : config_open));

    testutil_check(wiredtiger_open(home, &event_handler, config, &cfg->conn));
}

/*
 * wt_shutdown --
 *     Flush the file to disk and shut down the WiredTiger connection.
 */
static void
wt_shutdown(SHARED_CONFIG *cfg)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    conn = cfg->conn;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_check(session->checkpoint(session, NULL));

    testutil_check(conn->close(conn, NULL));
}

/*
 * shutdown --
 *     Clean up from previous runs.
 */
static void
shutdown(void)
{
    testutil_clean_work_dir(home);
}

/*
 * handle_error --
 *     TODO: Add a comment describing this function.
 */
static int
handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *errmsg)
{
    (void)(handler);
    (void)(session);
    (void)(error);

    return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

/*
 * handle_message --
 *     TODO: Add a comment describing this function.
 */
static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    (void)(handler);
    (void)(session);

    if (logfp != NULL)
        return (fprintf(logfp, "%s\n", message) < 0 ? -1 : 0);

    return (printf("%s\n", message) < 0 ? -1 : 0);
}

/*
 * onint --
 *     Interrupt signal handler.
 */
static void
onint(int signo)
{
    (void)(signo);

    shutdown();

    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

/*
 * usage --
 *     Display usage statement and exit failure.
 */
static int
usage(void)
{
    fprintf(stderr,
      "usage: %s "
      "[-FLv] [-C wiredtiger-config] [-k keys] [-l log]\n\t"
      "[-n ops] [-R reverse_scanners] [-r runs] [-t f|r|v] "
      "[-W append_inserters]\n",
      progname);
    fprintf(stderr, "%s",
      "\t-C specify wiredtiger_open configuration arguments\n"
      "\t-F create a file per thread\n"
      "\t-k set number of keys to load\n"
      "\t-L log print per operation\n"
      "\t-l specify a log file\n"
      "\t-n set number of operations each thread does\n"
      "\t-R set number of reverse scanner threads\n"
      "\t-r set number of runs (0 for continuous)\n"
      "\t-t set a file type (fix | row | var)\n"
      "\t-v do a different number of operations on different tables\n"
      "\t-w set number of items to walk in a reverse scan\n"
      "\t-W set number of threads doing append inserts\n");
    return (EXIT_FAILURE);
}
