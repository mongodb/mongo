/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2017-2022, Magnus Edenhill
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

#ifndef _RD_UNITTEST_H
#define _RD_UNITTEST_H

#include <stdio.h>


extern rd_bool_t rd_unittest_assert_on_failure;
extern rd_bool_t rd_unittest_on_ci;
extern rd_bool_t rd_unittest_slow;

#define ENABLE_CODECOV ENABLE_DEVEL


/**
 * @brief Begin single unit-test function (optional).
 *        Currently only used for logging.
 */
#define RD_UT_BEGIN()                                                          \
        fprintf(stderr, "\033[34mRDUT: INFO: %s:%d: %s: BEGIN: \033[0m\n",     \
                __FILE__, __LINE__, __FUNCTION__)


/**
 * @brief Fail the current unit-test function.
 */
#define RD_UT_FAIL(...)                                                        \
        do {                                                                   \
                fprintf(stderr, "\033[31mRDUT: FAIL: %s:%d: %s: ", __FILE__,   \
                        __LINE__, __FUNCTION__);                               \
                fprintf(stderr, __VA_ARGS__);                                  \
                fprintf(stderr, "\033[0m\n");                                  \
                if (rd_unittest_assert_on_failure)                             \
                        rd_assert(!*"unittest failure");                       \
                return 1;                                                      \
        } while (0)

/**
 * @brief Pass the current unit-test function
 */
#define RD_UT_PASS()                                                           \
        do {                                                                   \
                fprintf(stderr, "\033[32mRDUT: PASS: %s:%d: %s\033[0m\n",      \
                        __FILE__, __LINE__, __FUNCTION__);                     \
                return 0;                                                      \
        } while (0)

/**
 * @brief Skip the current unit-test function
 */
#define RD_UT_SKIP(...)                                                        \
        do {                                                                   \
                fprintf(stderr, "\033[33mRDUT: SKIP: %s:%d: %s: ", __FILE__,   \
                        __LINE__, __FUNCTION__);                               \
                fprintf(stderr, __VA_ARGS__);                                  \
                fprintf(stderr, "\033[0m\n");                                  \
                return 0;                                                      \
        } while (0)


/**
 * @brief Fail unit-test if \p expr is false
 */
#define RD_UT_ASSERT(expr, ...)                                                \
        do {                                                                   \
                if (!(expr)) {                                                 \
                        fprintf(stderr,                                        \
                                "\033[31mRDUT: FAIL: %s:%d: %s: "              \
                                "assert failed: " #expr ": ",                  \
                                __FILE__, __LINE__, __FUNCTION__);             \
                        fprintf(stderr, __VA_ARGS__);                          \
                        fprintf(stderr, "\033[0m\n");                          \
                        if (rd_unittest_assert_on_failure)                     \
                                rd_assert(expr);                               \
                        return 1;                                              \
                }                                                              \
        } while (0)


/**
 * @brief Check that value \p V is within inclusive range \p VMIN .. \p VMAX,
 *        else asserts.
 *
 * @param VFMT is the printf formatter for \p V's type
 */
#define RD_UT_ASSERT_RANGE(V, VMIN, VMAX, VFMT)                                \
        RD_UT_ASSERT((VMIN) <= (V) && (VMAX) >= (V),                           \
                     VFMT " out of range " VFMT " .. " VFMT, (V), (VMIN),      \
                     (VMAX))


/**
 * @brief Log something from a unit-test
 */
#define RD_UT_SAY(...)                                                         \
        do {                                                                   \
                fprintf(stderr, "RDUT: INFO: %s:%d: %s: ", __FILE__, __LINE__, \
                        __FUNCTION__);                                         \
                fprintf(stderr, __VA_ARGS__);                                  \
                fprintf(stderr, "\n");                                         \
        } while (0)


/**
 * @brief Warn about something from a unit-test
 */
#define RD_UT_WARN(...)                                                        \
        do {                                                                   \
                fprintf(stderr, "\033[33mRDUT: WARN: %s:%d: %s: ", __FILE__,   \
                        __LINE__, __FUNCTION__);                               \
                fprintf(stderr, __VA_ARGS__);                                  \
                fprintf(stderr, "\033[0m\n");                                  \
        } while (0)



int rd_unittest(void);



/**
 * @name Manual code coverage
 *
 * The RD_UT_COVERAGE*() set of macros are used to perform manual
 * code coverage testing.
 * This provides an alternative to object and state inspection by
 * instead verifying that certain code paths (typically error paths)
 * are executed, allowing functional black-box testing on the one part
 * combined with precise knowledge of code flow on the other part.
 *
 * How to use:
 *
 * 1. First identify a code path that you want to make sure is executed, such
 *    as a corner error case, increase RD_UT_COVNR_MAX (below) and use the
 *    new max number as the coverage number (COVNR).
 *
 * 2. In the code path add RD_UT_COVERAGE(your_covnr).
 *
 * 3. Write a unittest case that is supposed to trigger the code path.
 *
 * 4. In the unittest, add a call to RD_UT_COVERAGE_CHECK(your_covnr) at the
 *    point where you expect the code path to have executed.
 *
 * 5. RD_UT_COVERAGE_CHECK(your_covnr) will fail the current test, but not
 *    return from your test function, so you need to `return 1;` if
 *    RD_UT_COVERAGE_CHECK(your_covnr) returns 0, e.g:
 *
 *         if (!RD_UT_COVERAGE_CHECK(your_covnr))
 *              return 1; -- failure
 *
 * 6. Run the unit tests with `make unit` in tests/.
 *
 * 7. If the code path was not executed your test will fail, otherwise pass.
 *
 *
 * Code coverage checks require --enable-devel.
 *
 * There is a script in packaging/tools/rdutcoverage.sh that checks that
 * code coverage numbers are not reused.
 *
 * @{
 */

#if ENABLE_CODECOV

/* @define When adding new code coverages, use the next value and increment
 *         this maximum accordingly. */
#define RD_UT_COVNR_MAX 1

/**
 * @brief Register code as covered/executed.
 */
#define RD_UT_COVERAGE(COVNR)                                                  \
        rd_ut_coverage(__FILE__, __FUNCTION__, __LINE__, COVNR)

/**
 * @returns how many times the code was executed.
 *          will fail the unit test (but not return) if code has not
 *          been executed.
 */
#define RD_UT_COVERAGE_CHECK(COVNR)                                            \
        rd_ut_coverage_check(__FILE__, __FUNCTION__, __LINE__, COVNR)


void rd_ut_coverage(const char *file, const char *func, int line, int covnr);
int64_t
rd_ut_coverage_check(const char *file, const char *func, int line, int covnr);

#else

/* Does nothing if ENABLE_CODECOV is not set */
#define RD_UT_COVERAGE(COVNR)                                                  \
        do {                                                                   \
        } while (0)
#define RD_UT_COVERAGE_CHECK(COVNR) 1

#endif /* ENABLE_CODECOV */


/**@}*/


#endif /* _RD_UNITTEST_H */
