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
#include "test_util.h"

#include <signal.h>

/*
 * JIRA ticket reference: WT-2719 Test case description: Fuzz testing for WiredTiger
 * reconfiguration.
 */

static const char *const list[] = {",cache_overhead=13", ",cache_overhead=27", ",cache_overhead=8",

  ",cache_size=75MB", ",cache_size=214MB", ",cache_size=37MB",

  ",checkpoint=(log_size=104857600)",  /* 100MB */
  ",checkpoint=(log_size=1073741824)", /* 1GB */
  ",checkpoint=(log_size=2)", ",checkpoint=(log_size=0)", ",checkpoint=(wait=100)",
  ",checkpoint=(wait=10000)", ",checkpoint=(wait=2)", ",checkpoint=(wait=0)",

  ",compatibility=(release=2.6)", ",compatibility=(release=3.0)",

  ",error_prefix=\"prefix\"",

  ",eviction=(threads_min=7,threads_max=10)", ",eviction=(threads_min=17,threads_max=18)",
  ",eviction=(threads_min=3,threads_max=7)", ",eviction=(threads_max=12,threads_min=10)",
  ",eviction=(threads_max=18,threads_min=16)", ",eviction=(threads_max=10,threads_min=9)",

  ",eviction_dirty_target=45", ",eviction_dirty_target=87", ",eviction_dirty_target=8",

  ",eviction_dirty_trigger=37", ",eviction_dirty_trigger=98", ",eviction_dirty_trigger=7",

  ",eviction_target=22", ",eviction_target=84", ",eviction_target=30",

  ",eviction_trigger=75", ",eviction_trigger=95", ",eviction_trigger=66",

  ",file_manager=(close_handle_minimum=200)", ",file_manager=(close_handle_minimum=137)",
  ",file_manager=(close_handle_minimum=226)", ",file_manager=(close_idle_time=10000)",
  ",file_manager=(close_idle_time=12000)", ",file_manager=(close_idle_time=7)",
  ",file_manager=(close_idle_time=0)", ",file_manager=(close_scan_interval=50000)",
  ",file_manager=(close_scan_interval=59000)", ",file_manager=(close_scan_interval=3)",

  ",log=(archive=0)", ",log=(archive=1)", ",log=(prealloc=0)", ",log=(prealloc=1)",
  ",log=(zero_fill=0)", ",log=(zero_fill=1)",

  ",lsm_manager=(merge=0)", ",lsm_manager=(merge=1)", ",lsm_manager=(worker_thread_max=5)",
  ",lsm_manager=(worker_thread_max=18)", ",lsm_manager=(worker_thread_max=3)",

  ",shared_cache=(chunk=20MB)", ",shared_cache=(chunk=30MB)", ",shared_cache=(chunk=5MB)",
  ",shared_cache=(name=\"shared\")", ",shared_cache=(name=\"none\")", ",shared_cache=(quota=20MB)",
  ",shared_cache=(quota=30MB)", ",shared_cache=(quota=5MB)", ",shared_cache=(quota=0)",
  ",shared_cache=(reserve=20MB)", ",shared_cache=(reserve=30MB)", ",shared_cache=(reserve=5MB)",
  ",shared_cache=(reserve=0)", ",shared_cache=(size=100MB)", ",shared_cache=(size=1GB)",
  ",shared_cache=(size=75MB)",

  ",statistics=(\"all\")", ",statistics=(\"fast\")", ",statistics=(\"none\")",
  ",statistics=(\"all\",\"clear\")", ",statistics=(\"fast\",\"clear\")",

  ",statistics_log=(json=0)", ",statistics_log=(json=1)", ",statistics_log=(on_close=0)",
  ",statistics_log=(on_close=1)", ",statistics_log=(sources=(\"file:\"))",
  ",statistics_log=(sources=())", ",statistics_log=(timestamp=\"%b:%S\")",
  ",statistics_log=(timestamp=\"%H:%M\")", ",statistics_log=(wait=60)", ",statistics_log=(wait=76)",
  ",statistics_log=(wait=37)", ",statistics_log=(wait=0)",

  ",verbose=(\"api\")", ",verbose=(\"block\")", ",verbose=(\"checkpoint\")",
  ",verbose=(\"compact\")", ",verbose=(\"evict\")", ",verbose=(\"evictserver\")",
  ",verbose=(\"fileops\")", ",verbose=(\"handleops\")", ",verbose=(\"log\")", ",verbose=(\"lsm\")",
  ",verbose=(\"lsm_manager\")", ",verbose=(\"metadata\")", ",verbose=(\"mutex\")",
  ",verbose=(\"overflow\")", ",verbose=(\"read\")", ",verbose=(\"reconcile\")",
  ",verbose=(\"recovery\")", ",verbose=(\"salvage\")", ",verbose=(\"shared_cache\")",
  ",verbose=(\"split\")", ",verbose=(\"transaction\")", ",verbose=(\"verify\")",
  ",verbose=(\"version\")", ",verbose=(\"write\")", ",verbose=()"};

/*
 * handle_message --
 *     TODO: Add a comment describing this function.
 */
static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    (void)(handler);
    (void)(session);
    (void)(message);

    /* We configure verbose output, so just ignore. */
    return (0);
}

static WT_EVENT_HANDLER event_handler = {NULL, handle_message, NULL, NULL};

static const char *current; /* Current test configuration */

static void on_alarm(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
/*
 * on_alarm --
 *     TODO: Add a comment describing this function.
 */
static void
on_alarm(int signo)
{
    (void)signo; /* Unused parameter */

    fprintf(stderr, "configuration timed out: %s\n", current);
    abort();

    /* NOTREACHED */
}

/*
 * reconfig --
 *     TODO: Add a comment describing this function.
 */
static void
reconfig(TEST_OPTS *opts, WT_SESSION *session, const char *config)
{
    WT_DECL_RET;

    current = config;

    /*
     * Reconfiguration starts and stops servers, so hangs are more likely here than in other tests.
     * Don't let the test run too long and get a core dump when it happens.
     */
    (void)alarm(60);
    if ((ret = opts->conn->reconfigure(opts->conn, config)) != 0) {
        fprintf(stderr, "%s: %s\n", config, session->strerror(session, ret));
        exit(EXIT_FAILURE);
    }
    (void)alarm(0);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    enum { CACHE_SHARED, CACHE_SET, CACHE_NONE } cache;
    TEST_OPTS *opts, _opts;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    size_t len;
    u_int i, j;
    const char *p;
    char *config;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    opts->table_type = TABLE_ROW;
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, &event_handler, "create", &opts->conn));

    /* Open an LSM file so the LSM reconfiguration options make sense. */
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(session->create(session, opts->uri, "type=lsm,key_format=S,value_format=S"));

    /* Initialize the RNG. */
    __wt_random_init_seed(NULL, &rnd);

    /* Allocate memory for the config. */
    len = WT_ELEMENTS(list) * 64;
    config = dmalloc(len);

    /* Set an alarm so we can debug hangs. */
    (void)signal(SIGALRM, on_alarm);

    /* A linear pass through the list. */
    for (i = 0; i < WT_ELEMENTS(list); ++i)
        reconfig(opts, session, list[i]);

    /*
     * A linear pass through the list, adding random elements.
     *
     * WiredTiger configurations are usually "the last one set wins", but "shared_cache" and
     * "cache_set" options aren't allowed in the same configuration string.
     */
    for (i = 0; i < WT_ELEMENTS(list); ++i) {
        p = list[i];
        cache = CACHE_NONE;
        if (WT_PREFIX_MATCH(p, ",shared_cache"))
            cache = CACHE_SHARED;
        else if (WT_PREFIX_MATCH(p, ",cache_size"))
            cache = CACHE_SET;
        strcpy(config, p);

        for (j = (__wt_random(&rnd) % WT_ELEMENTS(list)) + 1; j > 0; --j) {
            p = list[__wt_random(&rnd) % WT_ELEMENTS(list)];
            if (WT_PREFIX_MATCH(p, ",shared_cache")) {
                if (cache == CACHE_SET)
                    continue;
                cache = CACHE_SHARED;
            } else if (WT_PREFIX_MATCH(p, ",cache_size")) {
                if (cache == CACHE_SHARED)
                    continue;
                cache = CACHE_SET;
            }
            strcat(config, p);
        }
        reconfig(opts, session, config);
    }

    /*
     * Turn on-close statistics off, if on-close is on and statistics were randomly turned off
     * during the run, close would fail.
     */
    testutil_check(opts->conn->reconfigure(opts->conn, "statistics_log=(on_close=0)"));

    free(config);
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
