// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/unittest/unittest.h"
#include "mongo/unittest/unittest_details.h"
#include "mongo/util/errno_util.h"  // IWYU pragme: keep (Used in macro expansion)
#include "mongo/util/modules.h"
#include "mongo/util/testing_proctor.h"  // IWYU pragma: keep (Used in macro expansion)

#include <functional>
#include <memory>
#include <string>

// Same as GTEST_TEST_CLASS_NAME_ but that isn't public.
#define DEATH_TEST_DETAIL_TEST_TYPE_NAME(SUITE_NAME, TEST_NAME) SUITE_NAME##_##TEST_NAME##_Test

/**
 * Constructs a single death test named `TEST_NAME` within the test suite `SUITE_NAME`.
 *
 * The test only succeeds if the process terminates abnormally, e.g. through an fassert
 * failure or deadly signal.
 *
 * Death tests are incompatible with already-running threads. If you need multiple threads
 * in your death test, start them in the test body, or use DEATH_TEST_F and start them
 * in the setUp() method of the fixture.
 *
 * DEATH_TESTs should use a "DeathTest" suffix (see
 * https://github.com/google/googletest/blob/main/docs/advanced.md#death-test-naming).
 */
#define DEATH_TEST(SUITE_NAME, TEST_NAME, MATCH_EXPR) \
    DEATH_TEST_DEFINE_(SUITE_NAME, TEST_NAME, MATCH_EXPR, ::mongo::unittest::Test)

/**
 * See DEATH_TEST for details.
 * Validates output is a partial match using a PCRE regular expression instead of a string match.
 */
#define DEATH_TEST_REGEX(SUITE_NAME, TEST_NAME, REGEX_EXPR) \
    DEATH_TEST_DEFINE_REGEX_(SUITE_NAME, TEST_NAME, REGEX_EXPR, ::mongo::unittest::Test)

/**
 * Constructs a single test named TEST_NAME that has access to a common fixture
 * named `FIXTURE_NAME`, which will be used as the Suite name.
 *
 * Fixture setUp, tearDown, ctor, and dtor run outside of any death checks.
 *
 * See description of DEATH_TEST for more details on death tests.
 */
#define DEATH_TEST_F(FIXTURE_NAME, TEST_NAME, MATCH_EXPR) \
    DEATH_TEST_DEFINE_(FIXTURE_NAME, TEST_NAME, MATCH_EXPR, FIXTURE_NAME)

#define DEATH_TEST_REGEX_F(FIXTURE_NAME, TEST_NAME, REGEX_EXPR) \
    DEATH_TEST_DEFINE_REGEX_(FIXTURE_NAME, TEST_NAME, REGEX_EXPR, FIXTURE_NAME)

#define DEATH_TEST_DEFINE_(SUITE_NAME, TEST_NAME, MATCH_EXPR, TEST_BASE)                  \
    DEATH_TEST_DEFINE_PRIMITIVE_(SUITE_NAME,                                              \
                                 TEST_NAME,                                               \
                                 MATCH_EXPR,                                              \
                                 ::testing::HasSubstr,                                    \
                                 DEATH_TEST_DETAIL_TEST_TYPE_NAME(SUITE_NAME, TEST_NAME), \
                                 TEST_BASE)

#define DEATH_TEST_DEFINE_REGEX_(SUITE_NAME, TEST_NAME, REGEX_EXPR, TEST_BASE)            \
    DEATH_TEST_DEFINE_PRIMITIVE_(SUITE_NAME,                                              \
                                 TEST_NAME,                                               \
                                 REGEX_EXPR,                                              \
                                 ::mongo::unittest::match::MatchesPcreRegex,              \
                                 DEATH_TEST_DETAIL_TEST_TYPE_NAME(SUITE_NAME, TEST_NAME), \
                                 TEST_BASE)


#define DEATH_TEST_DEFINE_PRIMITIVE_(                                                       \
    SUITE_NAME, TEST_NAME, MATCH_EXPR, MATCHER, TEST_TYPE, TEST_BASE)                       \
    class TEST_TYPE : public TEST_BASE {                                                    \
    public:                                                                                 \
        static int getLine() {                                                              \
            return __LINE__;                                                                \
        }                                                                                   \
                                                                                            \
        static std::string getFile() {                                                      \
            return __FILE__;                                                                \
        }                                                                                   \
                                                                                            \
        void _executeDeathTest() {                                                          \
            ASSERT_DEATH(_executeInChildForDeathTest(), MATCHER(MATCH_EXPR));               \
        }                                                                                   \
                                                                                            \
    private:                                                                                \
        void TestBody() override;                                                           \
                                                                                            \
        void _executeInChildForDeathTest() noexcept {                                       \
            SetUp();                                                                        \
            TestBody();                                                                     \
            TearDown();                                                                     \
            ::mongo::TestingProctor::instance().exitAbruptlyIfDeferredErrors();             \
        }                                                                                   \
                                                                                            \
        static inline auto _testInfo =                                                      \
            testing::RegisterTest(#SUITE_NAME,                                              \
                                  #TEST_NAME,                                               \
                                  nullptr,                                                  \
                                  nullptr,                                                  \
                                  __FILE__,                                                 \
                                  __LINE__,                                                 \
                                  []() -> ::mongo::unittest::DeathTestBase* {               \
                                      return new ::mongo::unittest::DeathTest<TEST_TYPE>(); \
                                  });                                                       \
    };                                                                                      \
    void TEST_TYPE::TestBody()

namespace mongo::unittest {

class [[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] DeathTestBase : public Test {
public:
    /**
     * A test can use this to opt-out of exec behavior.
     * Would have to be called by the constructor. By the time the test body is
     * running, it's too late.
     */
    void setExec(bool enable) {
        _exec = enable;
    }

protected:
    DeathTestBase() = default;

private:
    // Forks, executes _doMakeTest() in the child process to create a Test, then runs that Test.
    void TestBody() final;

    // Customization points for derived DeathTest classes.
    virtual int _getLine() = 0;
    virtual std::string _getFile() = 0;
    virtual void _executeDeathTest() = 0;

    /**
     * All death tests will fork a subprocess.
     * Some will be configured to then go ahead and exec.
     */
    bool _exec = true;
};

template <typename T>
class [[MONGO_MOD_PUBLIC]] DeathTest : public DeathTestBase {
public:
    template <typename... Args>
    explicit DeathTest(Args&&... args)
        : _makeTest([args...] { return std::make_unique<T>(args...); }) {}

private:
    int _getLine() override {
        return T::getLine();
    }

    std::string _getFile() override {
        return T::getFile();
    }

    void _executeDeathTest() override {
        return _makeTest()->_executeDeathTest();
    }

    class DeathTestStyleFlagGuard {
    public:
        DeathTestStyleFlagGuard() {
            GTEST_FLAG_SET(death_test_style, "threadsafe");
        }

        ~DeathTestStyleFlagGuard() {
            GTEST_FLAG_SET(death_test_style, _saved);
        }

    private:
        std::string _saved = GTEST_FLAG_GET(death_test_style);
    };

    DeathTestStyleFlagGuard _deathTestStyleFlagGuard;
    std::function<std::unique_ptr<T>()> _makeTest;
};

}  // namespace mongo::unittest
