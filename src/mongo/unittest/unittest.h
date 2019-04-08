/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
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
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest_helpers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

#define ASSERT_EQ(a, b) ASSERT_COMPARISON_(kEq, a, b)
#define ASSERT_NE(a, b) ASSERT_COMPARISON_(kNe, a, b)
#define ASSERT_LT(a, b) ASSERT_COMPARISON_(kLt, a, b)
#define ASSERT_LTE(a, b) ASSERT_COMPARISON_(kLe, a, b)
#define ASSERT_GT(a, b) ASSERT_COMPARISON_(kGt, a, b)
#define ASSERT_GTE(a, b) ASSERT_COMPARISON_(kGe, a, b)

/**
 * Binary comparison utility macro. Do not use directly.
 */
#define ASSERT_COMPARISON_(OP, a, b)                                                           \
    if (auto ca = ::mongo::unittest::ComparisonAssertion<::mongo::unittest::ComparisonOp::OP>( \
            __FILE__, __LINE__, #a, #b, a, b))                                                 \
    ca.failure().stream()

/**
 * Approximate equality assertion. Useful for comparisons on limited precision floating point
 * values.
 */
#define ASSERT_APPROX_EQUAL(a, b, ABSOLUTE_ERR) ASSERT_LTE(std::abs((a) - (b)), ABSOLUTE_ERR)

/**
 * Assert a function call returns its input unchanged.
 */
#define ASSERT_IDENTITY(INPUT, FUNCTION)                                                      \
    [&](auto&& v) {                                                                           \
        if (auto ca =                                                                         \
                ::mongo::unittest::ComparisonAssertion<::mongo::unittest::ComparisonOp::kEq>( \
                    __FILE__,                                                                 \
                    __LINE__,                                                                 \
                    #INPUT,                                                                   \
                    #FUNCTION "(" #INPUT ")",                                                 \
                    v,                                                                        \
                    FUNCTION(std::forward<decltype(v)>(v))))                                  \
            ca.failure().stream();                                                            \
    }(INPUT)

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
 * This internal helper is used to ignore warnings about unused results.  Some unit tests which test
 * `ASSERT_THROWS` and its variations are used on functions which both throw and return `Status` or
 * `StatusWith` objects.  Although such function designs are undesirable, they do exist, presently.
 * Therefore this internal helper macro is used by `ASSERT_THROWS` and its variations to silence
 * such warnings without forcing the caller to invoke `.ignore()` on the called function.
 *
 * NOTE: This macro should NOT be used inside regular unit test code to ignore unchecked `Status` or
 * `StatusWith` instances -- if a `Status` or `StatusWith` result is to be ignored, please use the
 * normal `.ignore()` code.  This macro exists only to make using `ASSERT_THROWS` less inconvenient
 * on functions which both throw and return `Status` or `StatusWith`.
 */
#define UNIT_TEST_INTERNALS_IGNORE_UNUSED_RESULT_WARNINGS(EXPRESSION) \
    do {                                                              \
        (void)(EXPRESSION);                                           \
    } while (false)

/**
 * Behaves like ASSERT_THROWS, above, but also calls CHECK(caughtException) which may contain
 * additional assertions.
 */
#define ASSERT_THROWS_WITH_CHECK(STATEMENT, EXCEPTION_TYPE, CHECK)             \
    do {                                                                       \
        try {                                                                  \
            UNIT_TEST_INTERNALS_IGNORE_UNUSED_RESULT_WARNINGS(STATEMENT);      \
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
            ::mongo::str::stream err;                                                           \
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

void setupTestLogger();

/**
 * Gets a LogstreamBuilder for logging to the unittest log domain, which may have
 * different target from the global log domain.
 */
mongo::logger::LogstreamBuilder log();
mongo::logger::LogstreamBuilder warning();

/**
 * Type representing the function composing a test.
 */
typedef stdx::function<void(void)> TestFunction;

/**
 * Container holding a test function and its name.  Suites
 * contain lists of these.
 */
class TestHolder {
    TestHolder(const TestHolder&) = delete;
    TestHolder& operator=(const TestHolder&) = delete;

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
    Test(const Test&) = delete;
    Test& operator=(const Test&) = delete;

public:
    Test();
    virtual ~Test();

    void run();

    /**
     * Called on the test object before running the test.
     */
    virtual void setUp() {}

    /**
     * Called on the test object after running the test.
     */
    virtual void tearDown() {}

protected:
    /**
     * Registration agent for adding tests to suites, used by TEST macro.
     */
    template <typename T>
    class RegistrationAgent {
        RegistrationAgent(const RegistrationAgent&) = delete;
        RegistrationAgent& operator=(const RegistrationAgent&) = delete;

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
     * The test itself.
     */
    virtual void _doTest() = 0;

    bool _isCapturingLogMessages;
    std::vector<std::string> _capturedLogMessages;
    logger::MessageLogDomain::AppenderHandle _captureAppenderHandle;
    std::unique_ptr<logger::MessageLogDomain::EventAppender> _captureAppender;
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
    Suite(const Suite&) = delete;
    Suite& operator=(const Suite&) = delete;

public:
    Suite(const std::string& name);
    virtual ~Suite();

    template <class T>
    void add() {
        add<T>(demangleName(typeid(T)));
    }

    template <class T, class A>
    void add(const A& a) {
        add(demangleName(typeid(T)), [a] {
            T testObj(a);
            testObj.run();
        });
    }

    template <class T>
    void add(const std::string& name) {
        add(name, [] {
            T testObj;
            testObj.run();
        });
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
    typedef std::vector<std::unique_ptr<TestHolder>> TestHolderList;

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

template <typename T>
Test::RegistrationAgent<T>::RegistrationAgent(const std::string& suiteName,
                                              const std::string& testName)
    : _suiteName(suiteName), _testName(testName) {
    Suite::getSuite(suiteName)->add<T>(testName);
}

template <typename T>
std::string Test::RegistrationAgent<T>::getSuiteName() const {
    return _suiteName;
}

template <typename T>
std::string Test::RegistrationAgent<T>::getTestName() const {
    return _testName;
}

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

    const std::string& getStacktrace() const {
        return _stacktrace;
    }

private:
    std::string _file;
    unsigned _line;
    std::string _message;
    std::string _stacktrace;
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

enum class ComparisonOp { kEq, kNe, kLt, kLe, kGt, kGe };

template <ComparisonOp op>
class ComparisonAssertion {
private:
    template <ComparisonOp val>
    using OpTag = std::integral_constant<ComparisonOp, val>;

    static auto comparator(OpTag<ComparisonOp::kEq>) {
        return [](auto&& a, auto&& b) { return a == b; };
    }
    static auto comparator(OpTag<ComparisonOp::kNe>) {
        return [](auto&& a, auto&& b) { return a != b; };
    }
    static auto comparator(OpTag<ComparisonOp::kLt>) {
        return [](auto&& a, auto&& b) { return a < b; };
    }
    static auto comparator(OpTag<ComparisonOp::kLe>) {
        return [](auto&& a, auto&& b) { return a <= b; };
    }
    static auto comparator(OpTag<ComparisonOp::kGt>) {
        return [](auto&& a, auto&& b) { return a > b; };
    }
    static auto comparator(OpTag<ComparisonOp::kGe>) {
        return [](auto&& a, auto&& b) { return a >= b; };
    }

    static constexpr StringData name(OpTag<ComparisonOp::kEq>) {
        return "=="_sd;
    }
    static constexpr StringData name(OpTag<ComparisonOp::kNe>) {
        return "!="_sd;
    }
    static constexpr StringData name(OpTag<ComparisonOp::kLt>) {
        return "<"_sd;
    }
    static constexpr StringData name(OpTag<ComparisonOp::kLe>) {
        return "<="_sd;
    }
    static constexpr StringData name(OpTag<ComparisonOp::kGt>) {
        return ">"_sd;
    }
    static constexpr StringData name(OpTag<ComparisonOp::kGe>) {
        return ">="_sd;
    }

public:
    template <typename A, typename B>
    ComparisonAssertion(const std::string& theFile,
                        unsigned theLine,
                        StringData aExpression,
                        StringData bExpression,
                        const A& a,
                        const B& b) {
        if (comparator(OpTag<op>{})(a, b)) {
            return;
        }
        std::ostringstream os;
        StringData opName = name(OpTag<op>{});
        os << "Expected " << aExpression << " " << opName << " " << bExpression << " (" << a << " "
           << opName << " " << b << ")";
        _assertion = std::make_unique<TestAssertionFailure>(theFile, theLine, os.str());
    }
    explicit operator bool() const {
        return static_cast<bool>(_assertion);
    }
    TestAssertionFailure failure() {
        return *_assertion;
    }

private:
    std::unique_ptr<TestAssertionFailure> _assertion;
};

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

}  // namespace unittest
}  // namespace mongo
