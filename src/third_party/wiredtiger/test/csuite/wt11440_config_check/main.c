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

/*
 * This program is both a simple test of some configuration code and a benchmark for
 * the same.  At the time of this program, we are actively looking at improving performance
 * of begin_transaction.  So, we measure the time taken for calling begin_transaction
 * different coding techniques that we'll call variants.  As we try out different approaches
 * we can add variants.  The "base" variant uses a configuration is formatted on each call.
 * One that is slightly better is where we choose which of many fixed configurations to use.
 *
 * For these variants, we are using a configuration string that has 4 variables,
 * which is exactly what MongoDB does.  Our strategy for measuring time is, for each
 * variant, do a large number (N_CALLS) of calls to begin_transaction
 * (end rollback_transaction), varying the parameters randomly.  We check that the
 * proper transaction flags are set after each begin_transaction call.  Before and after
 * the N_CALLS, we get system time and collect the difference.  So we do:
 *   TIME(N_CALLS of the first variant), TIME(N_CALLS of the second variant), etc.
 * We do that whole procedure a number of times (N_RUNS), and accumulate times for each
 * variant. This tends to smooth out any timing noise.
 *
 */
#define N_CALLS (WT_THOUSAND * 10)
#define N_RUNS (100)

#define IGNORE_PREPARE_VALUE_SIZE 3
static const char *ignore_prepare_value[3] = {"false", "force", "true"};
static const char *boolean_value[2] = {"false", "true"};

#define BEGIN_TRANSACTION_CONFIG_SPRINTF_FORMAT \
    "ignore_prepare=%s,roundup_timestamps=(prepared=%s,read=%s),no_timestamp=%s"

/* ================ VARIANT 0 - base ================ */

/*
 * begin_transaction_base_init --
 *     There is no initialization needed for the base implementation.
 */
static void
begin_transaction_base_init(WT_SESSION *session, void **datap)
{
    (void)session;
    *datap = NULL;
}

/*
 * begin_transaction_base --
 *     This is considered a typical implementation that a caller for begin_transaction will use. We
 *     incur the cost of formatting the configuration string on every call.
 */
static void
begin_transaction_base(WT_SESSION *session, void *data, u_int ignore_prepare, bool roundup_prepared,
  bool roundup_read, bool no_timestamp)
{
    char config[256];

    (void)data;
    testutil_check(__wt_snprintf(config, sizeof(config), BEGIN_TRANSACTION_CONFIG_SPRINTF_FORMAT,
      ignore_prepare_value[ignore_prepare], boolean_value[(int)roundup_prepared],
      boolean_value[(int)roundup_read], boolean_value[(int)no_timestamp]));
    testutil_check(session->begin_transaction(session, config));
}

/* ================ VARIANT 1 - advance format ================ */

/*
 * A faster implementation will take advantage of the finite number of configurations possible, and
 * we can do all formatting calls in advance.
 */
#define SPRINTF_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_ts) \
    (((((ignore_prepare * 2) + roundup_prepared) * 2) + roundup_read) * 2 + no_ts)

/*
 * begin_transaction_advance_format_init --
 *     Set up the static structures needed for this implementation.
 */
static void
begin_transaction_advance_format_init(WT_SESSION *session, void **datap)
{
    static char sprintf_config[3 * 2 * 2 * 2][256];
    static char *sprintf_config_array[3 * 2 * 2 * 2];

    (void)session;
    for (u_int ignore_prepare = 0; ignore_prepare < IGNORE_PREPARE_VALUE_SIZE; ++ignore_prepare)
        for (u_int roundup_prepared = 0; roundup_prepared < 2; ++roundup_prepared)
            for (u_int roundup_read = 0; roundup_read < 2; ++roundup_read)
                for (u_int no_ts = 0; no_ts < 2; ++no_ts) {
                    u_int entry =
                      SPRINTF_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_ts);
                    testutil_check(__wt_snprintf(sprintf_config[entry],
                      sizeof(sprintf_config[entry]), BEGIN_TRANSACTION_CONFIG_SPRINTF_FORMAT,
                      ignore_prepare_value[ignore_prepare], boolean_value[(int)roundup_prepared],
                      boolean_value[(int)roundup_read], boolean_value[(int)no_ts]));
                    sprintf_config_array[entry] = sprintf_config[entry];
                }
    *datap = &sprintf_config_array;
}

/*
 * begin_transaction_advance_format --
 *     An implementation of a begin_transaction caller, with a set of fixed config strings.
 */
static void
begin_transaction_advance_format(WT_SESSION *session, void *data, u_int ignore_prepare,
  bool roundup_prepared, bool roundup_read, bool no_timestamp)
{
    static char **sprintf_config;
    u_int entry;

    sprintf_config = data;
    entry = SPRINTF_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
    testutil_check(session->begin_transaction(session, sprintf_config[entry]));
}

/* ================ Table of variants ================ */

static struct impl {
    void (*begin_transaction_init_fcn)(WT_SESSION *, void **);
    void (*begin_transaction_fcn)(WT_SESSION *, void *, u_int, bool, bool, bool);
    void *init_data;
} impls[] = {
  {begin_transaction_base_init, begin_transaction_base, NULL},
  {begin_transaction_advance_format_init, begin_transaction_advance_format, NULL},
};
#define N_VARIANTS WT_ELEMENTS(impls)

/*
 * do_config_run --
 *     Run the test for the given variant.
 */
static void
do_config_run(TEST_OPTS *opts, u_int variant, bool check, uint64_t *nsec)
{
    struct timespec before, after;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    WT_TXN *txn;
    uint32_t r;
    u_int i, ignore_prepare;
    bool roundup_prepared, roundup_read, no_timestamp;
    void (*begin_transaction_fcn)(WT_SESSION *, void *, u_int, bool, bool, bool);
    void *init_data;

    session = opts->session;
    begin_transaction_fcn = impls[variant].begin_transaction_fcn;
    init_data = impls[variant].init_data;

    /* Initialize the RNG. */
    __wt_random_init_default(&rnd);

    __wt_epoch(NULL, &before);
    for (i = 0; i < N_CALLS; ++i) {
        r = __wt_random(&rnd);

        ignore_prepare = r % 3;
        roundup_prepared = ((r & 0x1) != 0);
        roundup_read = ((r & 0x2) != 0);
        no_timestamp = ((r & 0x4) != 0);

        begin_transaction_fcn(
          session, init_data, ignore_prepare, roundup_prepared, roundup_read, no_timestamp);

        if (check) {
            /*
             * Normal applications should not peer inside WT internals, but we need an easy way to
             * check that the configuration had the proper effect.
             */
            txn = ((WT_SESSION_IMPL *)session)->txn;
            if (ignore_prepare == 0) /* false */
                testutil_assert(
                  !F_ISSET(txn, WT_TXN_IGNORE_PREPARE) && !F_ISSET(txn, WT_TXN_READONLY));
            else if (ignore_prepare == 1) /* force */
                testutil_assert(
                  F_ISSET(txn, WT_TXN_IGNORE_PREPARE) && !F_ISSET(txn, WT_TXN_READONLY));
            else /* true */
                testutil_assert(
                  F_ISSET(txn, WT_TXN_IGNORE_PREPARE) && F_ISSET(txn, WT_TXN_READONLY));
            testutil_assert(roundup_prepared == F_ISSET(txn, WT_TXN_TS_ROUND_PREPARED));
            testutil_assert(roundup_read == F_ISSET(txn, WT_TXN_TS_ROUND_READ));
            testutil_assert(no_timestamp == F_ISSET(txn, WT_TXN_TS_NOT_SET));
        }

        testutil_check(session->rollback_transaction(session, NULL));
    }
    __wt_epoch(NULL, &after);
    *nsec +=
      (uint64_t)((after.tv_sec - before.tv_sec) * WT_BILLION + (after.tv_nsec - before.tv_nsec));
}

/*
 * main --
 *     The main entry point for a simple test/benchmark for the use of configuration strings.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    uint64_t base_ns, ns, nsecs[N_VARIANTS];
    u_int variant, runs;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_recreate_dir(opts->home);
    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,statistics=(all),statistics_log=(json,on_close,wait=1)", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &opts->session));

    /* Do any initialization needed by any of the variants. */
    for (variant = 0; variant < N_VARIANTS; ++variant)
        impls[variant].begin_transaction_init_fcn(opts->session, &impls[variant].init_data);

    memset(nsecs, 0, sizeof(nsecs));

    /* Run the test, alternating the variants of tests. */
    for (runs = 0; runs < N_RUNS; ++runs)
        for (variant = 0; variant < N_VARIANTS; ++variant)
            do_config_run(opts, variant, runs == 0, &nsecs[variant]);

    printf("number of calls: %d\n", N_CALLS * N_RUNS);
    base_ns = ns = nsecs[0] / (N_CALLS * N_RUNS);
    for (variant = 0; variant < N_VARIANTS; ++variant) {
        ns = nsecs[variant] / (N_CALLS * N_RUNS);
        printf("variant = %u, total = %" PRIu64
               ", nanoseconds per pair of begin/rollback calls = %" PRIu64
               ", speed vs baseline = %f\n",
          variant, nsecs[variant], ns, ((double)base_ns) / ns);
    }

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
