/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

/*
 * A C++ unit testing framework.
 *
 * For examples of basic usage, see mongo/unittest/unittest_test.cpp.
 */

#pragma once

#include <cmath>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

/**
 * Fail unconditionally, reporting the given message.
 */
#define FAIL(MESSAGE) ::mongo::unittest::TestAssertionFailure(__FILE__, __LINE__, MESSAGE).stream()

/**
 * Fails unless "EXPRESSION" is true.
 */
#define ASSERT_TRUE(EXPRESSION) \
    if (!(EXPRESSION))          \
    FAIL("Expected: " #EXPRESSION)
#define ASSERT(EXPRESSION) ASSERT_TRUE(EXPRESSION)

/**
 * Fails if "EXPRESSION" is true.
 */
#define ASSERT_FALSE(EXPRESSION) ASSERT(!(EXPRESSION))

/**
 * Asserts that a Status code is OK.
 */
#define ASSERT_OK(EXPRESSION) ASSERT_EQUALS(::mongo::Status::OK(), (EXPRESSION))

/**
 * Asserts that a status code is anything but OK.
 */
#define ASSERT_NOT_OK(EXPRESSION) ASSERT_NOT_EQUALS(::mongo::Status::OK(), (EXPRESSION))

/*
 * Binary comparison assertions.
 */
#define ASSERT_EQUALS(a, b) ASSERT_EQ(a, b)
#define ASSERT_NOT_EQUALS(a, b) ASSERT_NE(a, b)
#define ASSERT_LESS_THAN(a, b) ASSERT_LT(a, b)
#define ASSERT_NOT_LESS_THAN(a, b) ASSERT_GTE(a, b)
#define ASSERT_GREATER_THAN(a, b) ASSERT_GT(a, b)
#define ASSERT_NOT_GREATER_THAN(a, b) ASSERT_LTE(a, b)
#define ASSERT_LESS_THAN_OR_EQUALS(a, b) ASSERT_LTE(a, b)
#define ASSERT_GREATER_THAN_OR_EQUALS(a, b) ASSERT_GTE(a, b)

#define ASSERT_EQ(a, b) _ASSERT_COMPARISON(EQ, a, b)
#define ASSERT_NE(a, b) _ASSERT_COMPARISON(NE, a, b)
#define ASSERT_LT(a, b) _ASSERT_COMPARISON(LT, a, b)
#define ASSERT_LTE(a, b) _ASSERT_COMPARISON(LTE, a, b)
#define ASSERT_GT(a, b) _ASSERT_COMPARISON(GT, a, b)
#define ASSERT_GTE(a, b) _ASSERT_COMPARISON(GTE, a, b)

/**
 * Binary comparison utility macro.  Do not use directly.
 */
#define _ASSERT_COMPARISON(COMPARISON, a, b)                                                       \
    if (::mongo::unittest::ComparisonAssertion_##COMPARISON ca =                                   \
            ::mongo::unittest::ComparisonAssertion_##COMPARISON(__FILE__, __LINE__, #a, #b, a, b)) \
    ca.failure().stream()

/**
 * Approximate equality assertion. Useful for comparisons on limited precision floating point
 * values.
 */
#define ASSERT_APPROX_EQUAL(a, b, ABSOLUTE_ERR) ASSERT_LTE(std::abs((a) - (b)), ABSOLUTE_ERR)

/**
 * Verify that the evaluation of "EXPRESSION" throws an exception of type EXCEPTION_TYPE.
 *
 * If "EXPRESSION" throws no exception, or one that is neither of type "EXCEPTION_TYPE" nor
 * of a subtype of "EXCEPTION_TYPE", the test is considered a failure and further evaluation
 * halts.
 */
#define ASSERT_THROWS(STATEMENT, EXCEPTION_TYPE) \
    ASSERT_THROWS_WITH_CHECK(STATEMENT, EXCEPTION_TYPE, ([](const EXCEPTION_TYPE&) {}))

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling what() on the thrown exception
 * does not return a string equal to EXPECTED_WHAT.
 */
#define ASSERT_THROWS_WHAT(STATEMENT, EXCEPTION_TYPE, EXPECTED_WHAT)                     \
    ASSERT_THROWS_WITH_CHECK(STATEMENT, EXCEPTION_TYPE, ([&](const EXCEPTION_TYPE& ex) { \
                                 ASSERT_EQ(::mongo::StringData(ex.what()),               \
                                           ::mongo::StringData(EXPECTED_WHAT));          \
                             }))

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling getCode() on the thrown exception
 * does not return an error code equal to EXPECTED_CODE.
 */
#define ASSERT_THROWS_CODE(STATEMENT, EXCEPTION_TYPE, EXPECTED_CODE)                     \
    ASSERT_THROWS_WITH_CHECK(STATEMENT, EXCEPTION_TYPE, ([&](const EXCEPTION_TYPE& ex) { \
                                 ASSERT_EQ(ex.toStatus().code(), EXPECTED_CODE);         \
                             }))

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling getCode() on the thrown exception
 * does not return an error code equal to EXPECTED_CODE or if calling what() on the thrown exception
 * does not return a string equal to EXPECTED_WHAT.
 */
#define ASSERT_THROWS_CODE_AND_WHAT(STATEMENT, EXCEPTION_TYPE, EXPECTED_CODE, EXPECTED_WHAT) \
    ASSERT_THROWS_WITH_CHECK(STATEMENT, EXCEPTION_TYPE, ([&](const EXCEPTION_TYPE& ex) {     \
                                 ASSERT_EQ(ex.toStatus().code(), EXPECTED_CODE);             \
                                 ASSERT_EQ(::mongo::StringData(ex.what()),                   \
                                           ::mongo::StringData(EXPECTED_WHAT));              \
                             }))

/**
 * Behaves like ASSERT_THROWS, above, but also calls CHECK(caughtException) which may contain
 * additional assertions.
 */
#define ASSERT_THROWS_WITH_CHECK(STATEMENT, EXCEPTION_TYPE, CHECK)             \
    do {                                                                       \
        try {                                                                  \
            STATEMENT;                                                         \
            FAIL("Expected statement " #STATEMENT " to throw " #EXCEPTION_TYPE \
                 " but it threw nothing.");                                    \
        } catch (const EXCEPTION_TYPE& ex) {                                   \
            CHECK(ex);                                                         \
        }                                                                      \
    } while (false)

#define ASSERT_STRING_CONTAINS(BIG_STRING, CONTAINS)                                            \
    do {                                                                                        \
        std::string myString(BIG_STRING);                                                       \
        std::string myContains(CONTAINS);                                                       \
        if (myString.find(myContains) == std::string::npos) {                                   \
            ::mongoutils::str::stream err;                                                      \
            err << "Expected to find " #CONTAINS " (" << myContains << ") in " #BIG_STRING " (" \
                << myString << ")";                                                             \
            ::mongo::unittest::TestAssertionFailure(__FILE__, __LINE__, err).stream();          \
        }                                                                                       \
    } while (false)

/**
 * Construct a single test, named "TEST_NAME" within the test case "CASE_NAME".
 *
 * Usage:
 *
 * TEST(MyModuleTests, TestThatFooFailsOnErrors) {
 *     ASSERT_EQUALS(error_success, foo(invalidValue));
 * }
 */
#define TEST(CASE_NAME, TEST_NAME)                                                          \
    class _TEST_TYPE_NAME(CASE_NAME, TEST_NAME) : public ::mongo::unittest::Test {          \
    private:                                                                                \
        virtual void _doTest();                                                             \
                                                                                            \
        static const RegistrationAgent<_TEST_TYPE_NAME(CASE_NAME, TEST_NAME)> _agent;       \
    };                                                                                      \
    const ::mongo::unittest::Test::RegistrationAgent<_TEST_TYPE_NAME(CASE_NAME, TEST_NAME)> \
        _TEST_TYPE_NAME(CASE_NAME, TEST_NAME)::_agent(#CASE_NAME, #TEST_NAME);              \
    void _TEST_TYPE_NAME(CASE_NAME, TEST_NAME)::_doTest()

/**
 * Construct a single test named TEST_NAME that has access to a common class (a "fixture")
 * named "FIXTURE_NAME".
 *
 * Usage:
 *
 * class FixtureClass : public mongo::unittest::Test {
 * protected:
 *   int myVar;
 *   void setUp() { myVar = 10; }
 * };
 *
 * TEST(FixtureClass, TestThatUsesFixture) {
 *     ASSERT_EQUALS(10, myVar);
 * }
 */
#define TEST_F(FIXTURE_NAME, TEST_NAME)                                                        \
    class _TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME) : public FIXTURE_NAME {                     \
    private:                                                                                   \
        virtual void _doTest();                                                                \
                                                                                               \
        static const RegistrationAgent<_TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)> _agent;       \
    };                                                                                         \
    const ::mongo::unittest::Test::RegistrationAgent<_TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)> \
        _TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)::_agent(#FIXTURE_NAME, #TEST_NAME);           \
    void _TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)::_doTest()

/**
 * Macro to construct a type name for a test, from its "CASE_NAME" and "TEST_NAME".
 * Do not use directly in test code.
 */
#define _TEST_TYPE_NAME(CASE_NAME, TEST_NAME) UnitTest__##CASE_NAME##__##TEST_NAME

namespace mongo {

namespace unittest {

class Result;

/**
 * Gets a LogstreamBuilder for logging to the unittest log domain, which may have
 * different target from the global log domain.
 */
mongo::logger::LogstreamBuilder log();

/**
 * Type representing the function composing a test.
 */
typedef stdx::function<void(void)> TestFunction;

/**
 * Container holding a test function and its name.  Suites
 * contain lists of these.
 */
class TestHolder {
    MONGO_DISALLOW_COPYING(TestHolder);

public:
    TestHolder(const std::string& name, const TestFunction& fn) : _name(name), _fn(fn) {}

    ~TestHolder() {}
    void run() const {
        _fn();
    }
    std::string getName() const {
        return _name;
    }

private:
    std::string _name;
    TestFunction _fn;
};

/**
 * Base type for unit test fixtures.  Also, the default fixture type used
 * by the TEST() macro.
 */
class Test {
    MONGO_DISALLOW_COPYING(Test);

public:
    Test();
    virtual ~Test();

    void run();

protected:
    /**
     * Registration agent for adding tests to suites, used by TEST macro.
     */
    template <typename T>
    class RegistrationAgent {
        MONGO_DISALLOW_COPYING(RegistrationAgent);

    public:
        RegistrationAgent(const std::string& suiteName, const std::string& testName);
        std::string getSuiteName() const;
        std::string getTestName() const;

    private:
        const std::string _suiteName;
        const std::string _testName;
    };

    /**
     * This exception class is used to exercise the testing framework itself. If a test
     * case throws it, the framework would not consider it an error.
     */
    class FixtureExceptionForTesting : public std::exception {};

    /**
     * Starts capturing messages logged by code under test.
     *
     * Log messages will still also go to their default destination; this
     * code simply adds an additional sink for log messages.
     *
     * Clears any previously captured log lines.
     */
    void startCapturingLogMessages();

    /**
     * Stops capturing log messages logged by code under test.
     */
    void stopCapturingLogMessages();

    /**
     * Gets a vector of strings, one log line per string, captured since
     * the last call to startCapturingLogMessages() in this test.
     */
    const std::vector<std::string>& getCapturedLogMessages() const {
        return _capturedLogMessages;
    }

    /**
     * Returns the number of collected log lines containing "needle".
     */
    int64_t countLogLinesContaining(const std::string& needle);

    /**
     * Prints the captured log lines.
     */
    void printCapturedLogLines() const;

private:
    /**
     * Called on the test object before running the test.
     */
    virtual void setUp();

    /**
     * Called on the test object after running the test.
     */
    virtual void tearDown();

    /**
     * The test itself.
     */
    virtual void _doTest() = 0;

    bool _isCapturingLogMessages;
    std::vector<std::string> _capturedLogMessages;
    logger::MessageLogDomain::AppenderHandle _captureAppenderHandle;
    logger::MessageLogDomain::AppenderAutoPtr _captureAppender;
};

/**
 * Representation of a collection of tests.
 *
 * One suite is constructed for each "CASE_NAME" when using the TEST macro.
 * Additionally, tests that are part of dbtests are manually assigned to suites
 * by the programmer by overriding setupTests() in a subclass of Suite.  This
 * approach is deprecated.
 */
class Suite {
    MONGO_DISALLOW_COPYING(Suite);

public:
    Suite(const std::string& name);
    virtual ~Suite();

    template <class T>
    void add() {
        add<T>(demangleName(typeid(T)));
    }

    template <class T, typename A>
    void add(const A& a) {
        add(demangleName(typeid(T)), stdx::bind(&Suite::runTestObjectWithArg<T, A>, a));
    }

    template <class T>
    void add(const std::string& name) {
        add(name, &Suite::runTestObject<T>);
    }

    void add(const std::string& name, const TestFunction& testFn);

    Result* run(const std::string& filter, int runsPerTest);

    static int run(const std::vector<std::string>& suites,
                   const std::string& filter,
                   int runsPerTest);

    /**
     * Get a suite with the given name, creating it if necessary.
     *
     * The implementation of this function must be safe to call during the global static
     * initialization block before main() executes.
     */
    static Suite* getSuite(const std::string& name);

protected:
    virtual void setupTests();

private:
    // TODO(C++11): Make this hold unique_ptrs.
    typedef std::vector<std::shared_ptr<TestHolder>> TestHolderList;

    template <typename T>
    static void runTestObject() {
        T testObj;
        testObj.run();
    }

    template <typename T, typename A>
    static void runTestObjectWithArg(const A& a) {
        T testObj(a);
        testObj.run();
    }

    std::string _name;
    TestHolderList _tests;
    bool _ran;

    void registerSuite(const std::string& name, Suite* s);
};

// A type that makes it easy to declare a self registering suite for old style test
// declarations. Suites are self registering so this is *not* a memory leak.
template <typename T>
struct SuiteInstance {
    SuiteInstance() {
        new T;
    }

    template <typename U>
    SuiteInstance(const U& u) {
        new T(u);
    }
};

/**
 * Exception thrown when a test assertion fails.
 *
 * Typically thrown by helpers in the TestAssertion class and its ilk, below.
 *
 * NOTE(schwerin): This intentionally does _not_ extend std::exception, so that code under
 * test that (foolishly?) catches std::exception won't swallow test failures.  Doesn't
 * protect you from code that foolishly catches ..., but you do what you can.
 */
class TestAssertionFailureException {
public:
    TestAssertionFailureException(const std::string& theFile,
                                  unsigned theLine,
                                  const std::string& theMessage);

    const std::string& getFile() const {
        return _file;
    }
    unsigned getLine() const {
        return _line;
    }
    const std::string& getMessage() const {
        return _message;
    }
    void setMessage(const std::string& message) {
        _message = message;
    }

    const std::string& what() const {
        return getMessage();
    }

    std::string toString() const;

private:
    std::string _file;
    unsigned _line;
    std::string _message;
};

class TestAssertionFailure {
public:
    TestAssertionFailure(const std::string& file, unsigned line, const std::string& message);
    TestAssertionFailure(const TestAssertionFailure& other);
    ~TestAssertionFailure() noexcept(false);

    TestAssertionFailure& operator=(const TestAssertionFailure& other);

    std::ostream& stream();

private:
    TestAssertionFailureException _exception;
    std::ostringstream _stream;
    bool _enabled;
};

#define DECLARE_COMPARISON_ASSERTION(NAME, OPERATOR)                                          \
    class ComparisonAssertion_##NAME {                                                        \
        typedef void (ComparisonAssertion_##NAME::*bool_type)() const;                        \
                                                                                              \
    public:                                                                                   \
        template <typename A, typename B>                                                     \
        ComparisonAssertion_##NAME(const std::string& theFile,                                \
                                   unsigned theLine,                                          \
                                   StringData aExpression,                                    \
                                   StringData bExpression,                                    \
                                   const A& a,                                                \
                                   const B& b) {                                              \
            if (a OPERATOR b) {                                                               \
                return;                                                                       \
            }                                                                                 \
            std::ostringstream os;                                                            \
            os << "Expected " << aExpression << " " #OPERATOR " " << bExpression << " (" << a \
               << " " #OPERATOR " " << b << ")";                                              \
            _assertion.reset(new TestAssertionFailure(theFile, theLine, os.str()));           \
        }                                                                                     \
        operator bool_type() const {                                                          \
            return _assertion.get() ? &ComparisonAssertion_##NAME::comparison_failed : NULL;  \
        }                                                                                     \
        TestAssertionFailure failure() {                                                      \
            return *_assertion;                                                               \
        }                                                                                     \
                                                                                              \
    private:                                                                                  \
        void comparison_failed() const {}                                                     \
        std::shared_ptr<TestAssertionFailure> _assertion;                                     \
    }

DECLARE_COMPARISON_ASSERTION(EQ, ==);
DECLARE_COMPARISON_ASSERTION(NE, !=);
DECLARE_COMPARISON_ASSERTION(LT, <);
DECLARE_COMPARISON_ASSERTION(LTE, <=);
DECLARE_COMPARISON_ASSERTION(GT, >);
DECLARE_COMPARISON_ASSERTION(GTE, >=);
#undef DECLARE_COMPARISON_ASSERTION

/**
 * Get the value out of a StatusWith<T>, or throw an exception if it is not OK.
 */
template <typename T>
const T& assertGet(const StatusWith<T>& swt) {
    ASSERT_OK(swt.getStatus());
    return swt.getValue();
}

template <typename T>
T assertGet(StatusWith<T>&& swt) {
    ASSERT_OK(swt.getStatus());
    return std::move(swt.getValue());
}

/**
 * Return a list of suite names.
 */
std::vector<std::string> getAllSuiteNames();


inline bool alwaysTrue() {
    return true;
}

}  // namespace unittest
}  // namespace mongo

#include "mongo/unittest/unittest-inl.h"
