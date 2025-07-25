/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2017 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef _WIN32
#define RD_UNITTEST_QPC_OVERRIDES 1
#endif

#include "rd.h"
#include "rdunittest.h"

#include "rdvarint.h"
#include "rdbuf.h"
#include "crc32c.h"
#include "rdmurmur2.h"
#include "rdfnv1a.h"
#if WITH_HDRHISTOGRAM
#include "rdhdrhistogram.h"
#endif
#include "rdkafka_int.h"
#include "rdkafka_broker.h"
#include "rdkafka_request.h"

#include "rdsysqueue.h"
#include "rdkafka_sasl_oauthbearer.h"
#if WITH_OAUTHBEARER_OIDC
#include "rdkafka_sasl_oauthbearer_oidc.h"
#endif
#include "rdkafka_msgset.h"
#include "rdkafka_txnmgr.h"

rd_bool_t rd_unittest_assert_on_failure = rd_false;
rd_bool_t rd_unittest_on_ci             = rd_false;
rd_bool_t rd_unittest_slow              = rd_false;

#if ENABLE_CODECOV
/**
 * @name Code coverage
 * @{
 */

static rd_atomic64_t rd_ut_covnrs[RD_UT_COVNR_MAX + 1];

void rd_ut_coverage(const char *file, const char *func, int line, int covnr) {
        rd_assert(covnr >= 0 && covnr <= RD_UT_COVNR_MAX);
        rd_atomic64_add(&rd_ut_covnrs[covnr], 1);
}


int64_t
rd_ut_coverage_check(const char *file, const char *func, int line, int covnr) {
        int64_t r;

        rd_assert(covnr >= 0 && covnr <= RD_UT_COVNR_MAX);

        r = rd_atomic64_get(&rd_ut_covnrs[covnr]);

        if (!r) {
                fprintf(stderr,
                        "\033[31m"
                        "RDUT: FAIL: %s:%d: %s: "
                        "Code coverage nr %d: FAIL: "
                        "code path not executed: "
                        "perform `grep -RnF 'COVERAGE(%d)' src/` to find "
                        "source location"
                        "\033[0m\n",
                        file, line, func, covnr, covnr);
                if (rd_unittest_assert_on_failure)
                        rd_assert(!*"unittest failure");
                return 0;
        }

        fprintf(stderr,
                "\033[34mRDUT: CCOV: %s:%d: %s: Code coverage nr %d: "
                "PASS (%" PRId64 " code path execution(s))\033[0m\n",
                file, line, func, covnr, r);

        return r;
}
/**@}*/

#endif /* ENABLE_CODECOV */


/**
 * @name Test rdsysqueue.h / queue.h
 * @{
 */

struct ut_tq {
        TAILQ_ENTRY(ut_tq) link;
        int v;
};

TAILQ_HEAD(ut_tq_head, ut_tq);

struct ut_tq_args {
        const char *name; /**< Descriptive test name */
        struct {
                int base; /**< Base value */
                int cnt;  /**< Number of elements to add */
                int step; /**< Value step */
        } q[3];           /**< Queue element definition */
        int qcnt;         /**< Number of defs in .q */
        int exp[16];      /**< Expected value order after join */
};

/**
 * @brief Find the previous element (insert position) for
 *        value \p val in list \p head or NULL if \p val is less than
 *        the first element in \p head.
 * @remarks \p head must be ascending sorted.
 */
static struct ut_tq *ut_tq_find_prev_pos(const struct ut_tq_head *head,
                                         int val) {
        struct ut_tq *e, *prev = NULL;

        TAILQ_FOREACH(e, head, link) {
                if (e->v > val)
                        return prev;
                prev = e;
        }

        return prev;
}

static int ut_tq_test(const struct ut_tq_args *args) {
        int totcnt                = 0;
        int fails                 = 0;
        struct ut_tq_head *tqh[3] = {NULL, NULL, NULL};
        struct ut_tq *e, *insert_after;
        int i, qi;

        RD_UT_SAY("Testing TAILQ: %s", args->name);

        /*
         * Verify TAILQ_INSERT_LIST:
         *  For each insert position test:
         *  - create two lists: tqh 0 and 1
         *  - add entries to both lists
         *  - insert list 1 into 0
         *  - verify expected order and correctness
         */

        /* Use heap allocated heads to let valgrind/asan assist
         * in detecting corruption. */

        for (qi = 0; qi < args->qcnt; qi++) {
                tqh[qi] = rd_calloc(1, sizeof(*tqh[qi]));
                TAILQ_INIT(tqh[qi]);

                for (i = 0; i < args->q[qi].cnt; i++) {
                        e    = rd_malloc(sizeof(*e));
                        e->v = args->q[qi].base + (i * args->q[qi].step);
                        TAILQ_INSERT_TAIL(tqh[qi], e, link);
                }

                totcnt += args->q[qi].cnt;
        }

        for (qi = 1; qi < args->qcnt; qi++) {
                insert_after = ut_tq_find_prev_pos(tqh[0], args->q[qi].base);
                if (!insert_after) {
                        /* Insert position is head of list,
                         * do two-step concat+move */
                        TAILQ_PREPEND(tqh[0], tqh[qi], ut_tq_head, link);
                } else {
                        TAILQ_INSERT_LIST(tqh[0], insert_after, tqh[qi],
                                          ut_tq_head, struct ut_tq *, link);
                }

                RD_UT_ASSERT(TAILQ_EMPTY(tqh[qi]), "expected empty tqh[%d]",
                             qi);
                RD_UT_ASSERT(!TAILQ_EMPTY(tqh[0]), "expected non-empty tqh[0]");

                memset(tqh[qi], (int)'A', sizeof(*tqh[qi]));
                rd_free(tqh[qi]);
        }

        RD_UT_ASSERT(TAILQ_LAST(tqh[0], ut_tq_head)->v == args->exp[totcnt - 1],
                     "TAILQ_LAST val %d, expected %d",
                     TAILQ_LAST(tqh[0], ut_tq_head)->v, args->exp[totcnt - 1]);

        /* Add sentinel value to verify that INSERT_TAIL works
         * after INSERT_LIST */
        e    = rd_malloc(sizeof(*e));
        e->v = 99;
        TAILQ_INSERT_TAIL(tqh[0], e, link);
        totcnt++;

        i = 0;
        TAILQ_FOREACH(e, tqh[0], link) {
                if (i >= totcnt) {
                        RD_UT_WARN(
                            "Too many elements in list tqh[0]: "
                            "idx %d > totcnt %d: element %p (value %d)",
                            i, totcnt, e, e->v);
                        fails++;
                } else if (e->v != args->exp[i]) {
                        RD_UT_WARN(
                            "Element idx %d/%d in tqh[0] has value %d, "
                            "expected %d",
                            i, totcnt, e->v, args->exp[i]);
                        fails++;
                } else if (i == totcnt - 1 &&
                           e != TAILQ_LAST(tqh[0], ut_tq_head)) {
                        RD_UT_WARN("TAILQ_LAST == %p, expected %p",
                                   TAILQ_LAST(tqh[0], ut_tq_head), e);
                        fails++;
                }
                i++;
        }

        /* Then scan it in reverse */
        i = totcnt - 1;
        TAILQ_FOREACH_REVERSE(e, tqh[0], ut_tq_head, link) {
                if (i < 0) {
                        RD_UT_WARN(
                            "REVERSE: Too many elements in list tqh[0]: "
                            "idx %d < 0: element %p (value %d)",
                            i, e, e->v);
                        fails++;
                } else if (e->v != args->exp[i]) {
                        RD_UT_WARN(
                            "REVERSE: Element idx %d/%d in tqh[0] has "
                            "value %d, expected %d",
                            i, totcnt, e->v, args->exp[i]);
                        fails++;
                } else if (i == totcnt - 1 &&
                           e != TAILQ_LAST(tqh[0], ut_tq_head)) {
                        RD_UT_WARN("REVERSE: TAILQ_LAST == %p, expected %p",
                                   TAILQ_LAST(tqh[0], ut_tq_head), e);
                        fails++;
                }
                i--;
        }

        RD_UT_ASSERT(TAILQ_LAST(tqh[0], ut_tq_head)->v == args->exp[totcnt - 1],
                     "TAILQ_LAST val %d, expected %d",
                     TAILQ_LAST(tqh[0], ut_tq_head)->v, args->exp[totcnt - 1]);

        while ((e = TAILQ_FIRST(tqh[0]))) {
                TAILQ_REMOVE(tqh[0], e, link);
                rd_free(e);
        }

        rd_free(tqh[0]);

        return fails;
}


static int unittest_sysqueue(void) {
        const struct ut_tq_args args[] = {
            {"empty tqh[0]",
             {{0, 0, 0}, {0, 3, 1}},
             2,
             {0, 1, 2, 99 /*sentinel*/}},
            {"prepend 1,0",
             {{10, 3, 1}, {0, 3, 1}},
             2,
             {0, 1, 2, 10, 11, 12, 99}},
            {"prepend 2,1,0",
             {
                 {10, 3, 1}, /* 10, 11, 12 */
                 {5, 3, 1},  /* 5, 6, 7 */
                 {0, 2, 1}   /* 0, 1 */
             },
             3,
             {0, 1, 5, 6, 7, 10, 11, 12, 99}},
            {"insert 1", {{0, 3, 2}, {1, 2, 2}}, 2, {0, 1, 3, 2, 4, 99}},
            {"insert 1,2",
             {
                 {0, 3, 3}, /* 0, 3, 6 */
                 {1, 2, 3}, /* 1, 4 */
                 {2, 1, 3}  /* 2 */
             },
             3,
             {0, 1, 2, 4, 3, 6, 99}},
            {"append 1",
             {{0, 5, 1}, {5, 5, 1}},
             2,
             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 99}},
            {"append 1,2",
             {
                 {0, 5, 1}, /* 0, 1, 2, 3, 4 */
                 {5, 5, 1}, /* 5, 6, 7, 8, 9 */
                 {11, 2, 1} /* 11, 12 */
             },
             3,
             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 99}},
            {
                "insert 1,0,2",
                {
                    {5, 3, 1}, /* 5, 6, 7 */
                    {0, 1, 1}, /* 0 */
                    {10, 2, 1} /* 10, 11 */
                },
                3,
                {0, 5, 6, 7, 10, 11, 99},
            },
            {
                "insert 2,0,1",
                {
                    {5, 3, 1},  /* 5, 6, 7 */
                    {10, 2, 1}, /* 10, 11 */
                    {0, 1, 1}   /* 0 */
                },
                3,
                {0, 5, 6, 7, 10, 11, 99},
            },
            {NULL}};
        int i;
        int fails = 0;

        for (i = 0; args[i].name != NULL; i++)
                fails += ut_tq_test(&args[i]);

        RD_UT_ASSERT(!fails, "See %d previous failure(s)", fails);

        RD_UT_PASS();
}

/**@}*/


/**
 * @name rd_clock() unittests
 * @{
 */

#if RD_UNITTEST_QPC_OVERRIDES

/**
 * These values are based off a machine with freq 14318180
 * which would cause the original rd_clock() calculation to overflow
 * after about 8 days.
 * Details:
 * https://github.com/confluentinc/confluent-kafka-dotnet/issues/603#issuecomment-417274540
 */

static const int64_t rd_ut_qpc_freq = 14318180;
static int64_t rd_ut_qpc_now;

BOOL rd_ut_QueryPerformanceFrequency(_Out_ LARGE_INTEGER *lpFrequency) {
        lpFrequency->QuadPart = rd_ut_qpc_freq;
        return TRUE;
}

BOOL rd_ut_QueryPerformanceCounter(_Out_ LARGE_INTEGER *lpPerformanceCount) {
        lpPerformanceCount->QuadPart = rd_ut_qpc_now * rd_ut_qpc_freq;
        return TRUE;
}

static int unittest_rdclock(void) {
        rd_ts_t t1, t2;

        /* First let "uptime" be fresh boot (0). */
        rd_ut_qpc_now = 0;
        t1            = rd_clock();
        rd_ut_qpc_now++;
        t2 = rd_clock();
        RD_UT_ASSERT(t2 == t1 + (1 * 1000000),
                     "Expected t2 %" PRId64 " to be 1s more than t1 %" PRId64,
                     t2, t1);

        /* Then skip forward to 8 days, which should trigger the
         * overflow in a faulty implementation. */
        rd_ut_qpc_now = 8 * 86400;
        t2            = rd_clock();
        RD_UT_ASSERT(t2 == t1 + (8LL * 86400 * 1000000),
                     "Expected t2 %" PRId64
                     " to be 8 days larger than t1 %" PRId64,
                     t2, t1);

        /* And make sure we can run on a system with 38 years of uptime.. */
        rd_ut_qpc_now = 38 * 365 * 86400;
        t2            = rd_clock();
        RD_UT_ASSERT(t2 == t1 + (38LL * 365 * 86400 * 1000000),
                     "Expected t2 %" PRId64
                     " to be 38 years larger than t1 %" PRId64,
                     t2, t1);

        RD_UT_PASS();
}
#endif



/**@}*/

extern int unittest_string(void);
extern int unittest_cgrp(void);
#if WITH_SASL_SCRAM
extern int unittest_scram(void);
#endif
extern int unittest_assignors(void);
extern int unittest_map(void);
#if WITH_CURL
extern int unittest_http(void);
#endif
#if WITH_OAUTHBEARER_OIDC
extern int unittest_sasl_oauthbearer_oidc(void);
#endif

int rd_unittest(void) {
        int fails = 0;
        const struct {
                const char *name;
                int (*call)(void);
        } unittests[] = {
                {"sysqueue", unittest_sysqueue},
                {"string", unittest_string},
                {"map", unittest_map},
                {"rdbuf", unittest_rdbuf},
                {"rdvarint", unittest_rdvarint},
                {"crc32c", unittest_rd_crc32c},
                {"msg", unittest_msg},
                {"murmurhash", unittest_murmur2},
                {"fnv1a", unittest_fnv1a},
#if WITH_HDRHISTOGRAM
                {"rdhdrhistogram", unittest_rdhdrhistogram},
#endif
#ifdef _WIN32
                {"rdclock", unittest_rdclock},
#endif
                {"conf", unittest_conf},
                {"broker", unittest_broker},
                {"request", unittest_request},
#if WITH_SASL_OAUTHBEARER
                {"sasl_oauthbearer", unittest_sasl_oauthbearer},
#endif
                {"aborted_txns", unittest_aborted_txns},
                {"cgrp", unittest_cgrp},
#if WITH_SASL_SCRAM
                {"scram", unittest_scram},
#endif
                {"assignors", unittest_assignors},
#if WITH_CURL
                {"http", unittest_http},
#endif
#if WITH_OAUTHBEARER_OIDC
                {"sasl_oauthbearer_oidc", unittest_sasl_oauthbearer_oidc},
#endif
                {NULL}
        };
        int i;
        const char *match = rd_getenv("RD_UT_TEST", NULL);
        int cnt           = 0;

        if (rd_getenv("RD_UT_ASSERT", NULL))
                rd_unittest_assert_on_failure = rd_true;
        if (rd_getenv("CI", NULL)) {
                RD_UT_SAY("Unittests running on CI");
                rd_unittest_on_ci = rd_true;
        }

        if (rd_unittest_on_ci || (ENABLE_DEVEL + 0)) {
                RD_UT_SAY("Unittests will not error out on slow CPUs");
                rd_unittest_slow = rd_true;
        }

        rd_kafka_global_init();

#if ENABLE_CODECOV
        for (i = 0; i < RD_UT_COVNR_MAX + 1; i++)
                rd_atomic64_init(&rd_ut_covnrs[i], 0);
#endif

        for (i = 0; unittests[i].name; i++) {
                int f;

                if (match && !strstr(unittests[i].name, match))
                        continue;

                f = unittests[i].call();
                RD_UT_SAY("unittest: %s: %4s\033[0m", unittests[i].name,
                          f ? "\033[31mFAIL" : "\033[32mPASS");
                fails += f;
                cnt++;
        }

#if ENABLE_CODECOV
#if FIXME /* This check only works if all tests that use coverage checks       \
           * are run, which we can't really know, so disable until we          \
           * know what to do with this. */
        if (!match) {
                /* Verify all code paths were covered */
                int cov_fails = 0;
                for (i = 0; i < RD_UT_COVNR_MAX + 1; i++) {
                        if (!RD_UT_COVERAGE_CHECK(i))
                                cov_fails++;
                }
                if (cov_fails > 0)
                        RD_UT_SAY("%d code coverage failure(s) (ignored)\n",
                                  cov_fails);
        }
#endif
#endif

        if (!cnt && match)
                RD_UT_WARN("No unittests matching \"%s\"", match);

        return fails;
}
