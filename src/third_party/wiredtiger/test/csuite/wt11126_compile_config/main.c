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
 * This program is both a simple test of precompiling configurations and a benchmark
 * for the same.  At the time of this program, we only support precompiling for
 * begin_transaction.  So, we measure the time taken for calling begin_transaction
 * different coding techniques that we'll call variants.  These variants range from a
 * naive approach, where the configuration is formatted on each call, to slightly better, where we
 * choose which of many fixed configurations to use.  Next is using precompiling,
 * and "binding" the values on each call.  Finally, for completeness, we test having
 * many fixed configurations that are precompiled.
 *
 * For all of these variants, we are using a configuration string that has 4 variables,
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
#define N_VARIANTS 5

/* Description of each variant. */
static const char *descriptions[N_VARIANTS] = {
  "baseline, formats a configuration string each time",
  "each call chooses a pre-made configuration string",
  "each call uses a precompiled string, and uses bindings",
  "each call chooses a precompiled configuration string",
  "each call has a null configuration",
};

#define IGNORE_PREPARE_VALUE_SIZE 3
static const char *ignore_prepare_value[3] = {"false", "force", "true"};
static const char *boolean_value[2] = {"false", "true"};

#define BEGIN_TRANSACTION_CONFIG_PRECOMPILE_FORMAT \
    "ignore_prepare=%s,roundup_timestamps=(prepared=%d,read=%d),no_timestamp=%d"

#define BEGIN_TRANSACTION_CONFIG_PRINTF_FORMAT \
    "ignore_prepare=%s,roundup_timestamps=(prepared=%s,read=%s),no_timestamp=%s"

/*
 * begin_transaction_slow --
 *     This is considered a typical implementation that a caller for begin_transaction will use. We
 *     incur the cost of formatting the configuration string on every call.
 */
static void
begin_transaction_slow(WT_SESSION *session, u_int ignore_prepare, bool roundup_prepared,
  bool roundup_read, bool no_timestamp)
{
    char config[256];

    testutil_check(__wt_snprintf(config, sizeof(config), BEGIN_TRANSACTION_CONFIG_PRINTF_FORMAT,
      ignore_prepare_value[ignore_prepare], boolean_value[(int)roundup_prepared],
      boolean_value[(int)roundup_read], boolean_value[(int)no_timestamp]));
    testutil_check(session->begin_transaction(session, config));
}

/*
 * A faster implementation will take advantage of the finite number of configurations possible. It
 * requires an initialization step.
 */
static char medium_config[3 * 2 * 2 * 2][256];
#define MEDIUM_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_ts) \
    (((((ignore_prepare * 2) + roundup_prepared) * 2) + roundup_read) * 2 + no_ts)

/*
 * begin_transaction_medium_init --
 *     Set up the static structures needed for a medium level implementation.
 */
static void
begin_transaction_medium_init(void)
{
    for (u_int ignore_prepare = 0; ignore_prepare < IGNORE_PREPARE_VALUE_SIZE; ++ignore_prepare)
        for (u_int roundup_prepared = 0; roundup_prepared < 2; ++roundup_prepared)
            for (u_int roundup_read = 0; roundup_read < 2; ++roundup_read)
                for (u_int no_ts = 0; no_ts < 2; ++no_ts) {
                    u_int entry =
                      MEDIUM_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_ts);
                    testutil_check(__wt_snprintf(medium_config[entry], sizeof(medium_config[entry]),
                      BEGIN_TRANSACTION_CONFIG_PRINTF_FORMAT, ignore_prepare_value[ignore_prepare],
                      boolean_value[(int)roundup_prepared], boolean_value[(int)roundup_read],
                      boolean_value[(int)no_ts]));
                }
}

/*
 * begin_transaction_medium --
 *     A medium implementation of a begin_transaction caller, with a set of fixed config strings.
 */
static void
begin_transaction_medium(WT_SESSION *session, u_int ignore_prepare, bool roundup_prepared,
  bool roundup_read, bool no_timestamp)
{
    u_int entry;

    entry = MEDIUM_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
    testutil_check(session->begin_transaction(session, medium_config[entry]));
}

/*
 * A still faster implementation will require WiredTiger to be involved in the precompilation. It
 * requires an initialization step that needs to be run after wiredtiger_open and creates a
 * precompiled string that is valid for the life of the connection. To be used, the parameters need
 * to be bound with a separate call.
 */

/*
 * begin_transaction_fast_init --
 *     Set up the precompilation needed for a fast implementation.
 */
static void
begin_transaction_fast_init(WT_CONNECTION *conn, const char **compiled_ptr)
{
    testutil_check(conn->compile_configuration(conn, "WT_SESSION.begin_transaction",
      BEGIN_TRANSACTION_CONFIG_PRECOMPILE_FORMAT, compiled_ptr));
}

/*
 * begin_transaction_fast --
 *     A fast implementation of a begin_transaction caller, using compiled config strings.
 *     Parameters must be bound before the API call.
 */
static void
begin_transaction_fast(WT_SESSION *session, const char *compiled, u_int ignore_prepare,
  bool roundup_prepared, bool roundup_read, bool no_timestamp)
{
    testutil_check(session->bind_configuration(session, compiled,
      ignore_prepare_value[ignore_prepare], roundup_prepared, roundup_read, no_timestamp));
    testutil_check(session->begin_transaction(session, compiled));
}

/*
 * Another fast implementation takes advantage of the finite number of configuration strings, and
 * calls the WiredTiger configuration compiler to get a precompiled string for each one.
 */

/*
 * begin_transaction_fast_alternate_init --
 *     Set up the precompilation and fixed strings needed for a fast implementation.
 */
static void
begin_transaction_fast_alternate_init(WT_CONNECTION *conn, const char ***compiled_array_ptr)
{
    static const char *compiled_config[3 * 2 * 2 * 2];
#define MANY_COMPILED_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_ts) \
    (((((ignore_prepare * 2) + roundup_prepared) * 2) + roundup_read) * 2 + no_ts)

    char config[256];

    for (u_int ignore_prepare = 0; ignore_prepare < IGNORE_PREPARE_VALUE_SIZE; ++ignore_prepare)
        for (u_int roundup_prepared = 0; roundup_prepared < 2; ++roundup_prepared)
            for (u_int roundup_read = 0; roundup_read < 2; ++roundup_read)
                for (u_int no_ts = 0; no_ts < 2; ++no_ts) {
                    u_int entry =
                      MANY_COMPILED_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_ts);
                    testutil_check(
                      __wt_snprintf(config, sizeof(config), BEGIN_TRANSACTION_CONFIG_PRINTF_FORMAT,
                        ignore_prepare_value[ignore_prepare], boolean_value[(int)roundup_prepared],
                        boolean_value[(int)roundup_read], boolean_value[(int)no_ts]));
                    testutil_check(conn->compile_configuration(
                      conn, "WT_SESSION.begin_transaction", config, &compiled_config[entry]));
                }
    *compiled_array_ptr = compiled_config;
}

/*
 * begin_transaction_fast_alternate --
 *     A fast implementation of a begin_transaction caller, with compiled config strings that have
 *     fixed parameters. This skips any need to bind parameters.
 */
static void
begin_transaction_fast_alternate(WT_SESSION *session, const char **compiled_array,
  u_int ignore_prepare, bool roundup_prepared, bool roundup_read, bool no_timestamp)
{
    u_int entry;

    entry = MANY_COMPILED_ENTRY(ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
    testutil_check(session->begin_transaction(session, compiled_array[entry]));
}

/*
 * begin_transaction_null --
 *     This does a begin transaction without any parameters, merely for comparison benchmarks.
 */
static void
begin_transaction_null(WT_SESSION *session)
{
    testutil_check(session->begin_transaction(session, NULL));
}

/*
 * do_config_run --
 *     Run the test with or without configuration compilation.
 */
static void
do_config_run(TEST_OPTS *opts, int variant, const char *compiled, const char **compiled_array,
  bool check, uint64_t *nsec)
{
    struct timespec before, after;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    WT_TXN *txn;
    uint32_t r;
    u_int i, ignore_prepare;
    bool roundup_prepared, roundup_read, no_timestamp;

    session = opts->session;

    /* Initialize the RNG. */
    __wt_random_init(&rnd);

    __wt_epoch(NULL, &before);
    for (i = 0; i < N_CALLS; ++i) {
        r = __wt_random(&rnd);

        ignore_prepare = r % 3;
        roundup_prepared = ((r & 0x1) != 0);
        roundup_read = ((r & 0x2) != 0);
        no_timestamp = ((r & 0x4) != 0);

        switch (variant) {
        case 0:
            begin_transaction_slow(
              session, ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
            break;
        case 1:
            begin_transaction_medium(
              session, ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
            break;
        case 2:
            begin_transaction_fast(
              session, compiled, ignore_prepare, roundup_prepared, roundup_read, no_timestamp);
            break;
        case 3:
            begin_transaction_fast_alternate(session, compiled_array, ignore_prepare,
              roundup_prepared, roundup_read, no_timestamp);
            break;
        case 4:
            /*
             * We always have a null configuration, so we are not setting the "right" parameters.
             * This one cannot be checked, it is only for comparison benchmarks.
             */
            begin_transaction_null(session);
            check = false;
            break;
        default:
            testutil_assert(variant < N_VARIANTS);
            break;
        }

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
 *     The main entry point for a simple test/benchmark for compiling configuration strings.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    uint64_t base_ns, ns, nsecs[N_VARIANTS];
    int variant, runs;
    const char *compiled_config, **compiled_config_array;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_recreate_dir(opts->home);
    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,statistics=(all),statistics_log=(json,on_close,wait=1)", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &opts->session));

    begin_transaction_medium_init();
    begin_transaction_fast_init(opts->conn, &compiled_config);
    begin_transaction_fast_alternate_init(opts->conn, &compiled_config_array);

    memset(nsecs, 0, sizeof(nsecs));

    /* Run the test, alternating the variants of tests. */
    for (runs = 0; runs < N_RUNS; ++runs)
        for (variant = 0; variant < N_VARIANTS; ++variant) {
            do_config_run(
              opts, variant, compiled_config, compiled_config_array, runs == 0, &nsecs[variant]);
        }

    printf("number of calls: %d\n", N_CALLS * N_RUNS);
    base_ns = ns = nsecs[0] / (N_CALLS * N_RUNS);
    for (variant = 0; variant < N_VARIANTS; ++variant) {
        ns = nsecs[variant] / (N_CALLS * N_RUNS);
        printf("variant %d: %s, nsec per begin/rollback pair = %" PRIu64 ", vs baseline = %f\n",
          variant, descriptions[variant], ns, ((double)base_ns) / ns);
    }

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
