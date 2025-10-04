/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
 * ASSERTion macros for the C++ unit testing framework.
 */

#pragma once

// IWYU pragma: private, include "mongo/unittest/unittest.h"
// IWYU pragma: friend "mongo/unittest/.*"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/mutable_bson/mutable_bson_test_utils.h"
#include "mongo/logv2/log_debug.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/stringify.h"
#include "mongo/unittest/test_info.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/str.h"

#include <cmath>
#include <functional>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

/**
 * Fail unconditionally, reporting the given message.
 */
#define FAIL(MESSAGE) ::mongo::unittest::TestAssertionFailure(__FILE__, __LINE__, MESSAGE).stream()

/**
 * Fails unless "EXPRESSION" is true.
 */
#define ASSERT_TRUE(EXPRESSION) \
    if (EXPRESSION) {           \
    } else                      \
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
#define ASSERT_COMPARISON_(OP, a, b) ASSERT_COMPARISON_STR_(OP, a, b, #a, #b)

#define ASSERT_COMPARISON_STR_(OP, a, b, aExpr, bExpr)                                         \
    if (auto ca =                                                                              \
            ::mongo::unittest::ComparisonAssertion<::mongo::unittest::ComparisonOp::OP>::make( \
                __FILE__, __LINE__, aExpr, bExpr, a, b);                                       \
        !ca) {                                                                                 \
    } else                                                                                     \
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
#define ASSERT_THROWS(EXPRESSION, EXCEPTION_TYPE) \
    ASSERT_THROWS_WITH_CHECK(EXPRESSION, EXCEPTION_TYPE, ([](const EXCEPTION_TYPE&) {}))

/**
 * Verify that the evaluation of "EXPRESSION" does not throw any exceptions.
 *
 * If "EXPRESSION" throws an exception the test is considered a failure and further evaluation
 * halts.
 */
#define ASSERT_DOES_NOT_THROW(EXPRESSION)                          \
    try {                                                          \
        EXPRESSION;                                                \
    } catch (const AssertionException& e) {                        \
        str::stream err;                                           \
        err << "Threw an exception incorrectly: " << e.toString(); \
        FAIL(err);                                                 \
    }

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling what() on the thrown exception
 * does not return a string equal to EXPECTED_WHAT.
 */
#define ASSERT_THROWS_WHAT(EXPRESSION, EXCEPTION_TYPE, EXPECTED_WHAT)                     \
    ASSERT_THROWS_WITH_CHECK(EXPRESSION, EXCEPTION_TYPE, ([&](const EXCEPTION_TYPE& ex) { \
                                 ASSERT_EQ(::mongo::StringData(ex.what()),                \
                                           ::mongo::StringData(EXPECTED_WHAT));           \
                             }))

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling getCode() on the thrown exception
 * does not return an error code equal to EXPECTED_CODE.
 */
#define ASSERT_THROWS_CODE(EXPRESSION, EXCEPTION_TYPE, EXPECTED_CODE)                     \
    ASSERT_THROWS_WITH_CHECK(EXPRESSION, EXCEPTION_TYPE, ([&](const EXCEPTION_TYPE& ex) { \
                                 ASSERT_EQ(ex.toStatus().code(), EXPECTED_CODE);          \
                             }))

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling getCode() on the thrown exception
 * does not return an error code equal to EXPECTED_CODE or if calling what() on the thrown exception
 * does not return a string equal to EXPECTED_WHAT.
 */
#define ASSERT_THROWS_CODE_AND_WHAT(EXPRESSION, EXCEPTION_TYPE, EXPECTED_CODE, EXPECTED_WHAT) \
    ASSERT_THROWS_WITH_CHECK(EXPRESSION, EXCEPTION_TYPE, ([&](const EXCEPTION_TYPE& ex) {     \
                                 ASSERT_EQ(ex.toStatus().code(), EXPECTED_CODE);              \
                                 ASSERT_EQ(::mongo::StringData(ex.what()),                    \
                                           ::mongo::StringData(EXPECTED_WHAT));               \
                             }))


/**
 * Compiles if expr doesn't compile.
 *
 * This only works for compile errors in the "immediate context" of the expression, which matches
 * the rules for SFINAE. The first argument is a defaulted template parameter that is used in the
 * expression to make it dependent. This only works with expressions, not statements, although you
 * can separate multiple expressions with a comma.
 *
 * This should be used at namespace scope, not inside a TEST function.
 *
 * Examples that pass:
 *     ASSERT_DOES_NOT_COMPILE(MyTest1, typename Char = char, *std::declval<Char>());
 *     ASSERT_DOES_NOT_COMPILE(MyTest2, bool B = false, std::enable_if_t<B, int>{});
 *
 * Examples that fail:
 *     ASSERT_DOES_NOT_COMPILE(MyTest3, typename Char = char, *std::declval<Char*>());
 *     ASSERT_DOES_NOT_COMPILE(MyTest4, bool B = true, std::enable_if_t<B, int>{});
 *
 */
#define ASSERT_DOES_NOT_COMPILE(Id, Alias, ...) \
    ASSERT_DOES_NOT_COMPILE_1_(Id, Alias, #Alias, (__VA_ARGS__), #__VA_ARGS__)

#define ASSERT_DOES_NOT_COMPILE_1_(Id, Alias, AliasString, Expr, ExprString)  \
                                                                              \
    static std::true_type Id(...);                                            \
                                                                              \
    template <Alias>                                                          \
    static std::conditional_t<true, std::false_type, decltype(Expr)> Id(int); \
                                                                              \
    static_assert(decltype(Id(0))::value,                                     \
                  "Expression '" ExprString "' [with " AliasString "] shouldn't compile.");

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
#define ASSERT_THROWS_WITH_CHECK(EXPRESSION, EXCEPTION_TYPE, CHECK)                \
    if ([&] {                                                                      \
            try {                                                                  \
                UNIT_TEST_INTERNALS_IGNORE_UNUSED_RESULT_WARNINGS(EXPRESSION);     \
                return false;                                                      \
            } catch (const EXCEPTION_TYPE& ex) {                                   \
                CHECK(ex);                                                         \
                return true;                                                       \
            }                                                                      \
        }()) {                                                                     \
    } else                                                                         \
        /* Fail outside of the try/catch, this way the code in the `FAIL` macro */ \
        /* doesn't have the potential to throw an exception which we might also */ \
        /* be checking for. */                                                     \
        FAIL("Expected expression " #EXPRESSION " to throw " #EXCEPTION_TYPE       \
             " but it threw nothing.")

#define ASSERT_STRING_CONTAINS(BIG_STRING, CONTAINS)                                           \
    if (auto tup_ = std::tuple(std::string(BIG_STRING), std::string(CONTAINS));                \
        std::get<0>(tup_).find(std::get<1>(tup_)) != std::string::npos) {                      \
    } else                                                                                     \
        FAIL(([&] {                                                                            \
            const auto& [haystack, sub] = tup_;                                                \
            return fmt::format(                                                                \
                "Expected to find {} ({}) in {} ({})", #CONTAINS, sub, #BIG_STRING, haystack); \
        }()))

#define ASSERT_STRING_OMITS(BIG_STRING, OMITS)                               \
    if (auto tup_ = std::tuple(std::string(BIG_STRING), std::string(OMITS)); \
        std::get<0>(tup_).find(std::get<1>(tup_)) == std::string::npos) {    \
    } else                                                                   \
        FAIL(([&] {                                                          \
            const auto& [haystack, omits] = tup_;                            \
            return fmt::format("Did not expect to find {} ({}) in {} ({})",  \
                               #OMITS,                                       \
                               omits,                                        \
                               #BIG_STRING,                                  \
                               haystack);                                    \
        }()))

#define ASSERT_STRING_SEARCH_REGEX(BIG_STRING, REGEX)                                        \
    if (auto tup_ = std::tuple(std::string(BIG_STRING), mongo::pcre::Regex(REGEX));          \
        ::mongo::unittest::searchRegex(std::get<1>(tup_), std::get<0>(tup_))) {              \
    } else                                                                                   \
        FAIL(([&] {                                                                          \
            const auto& [haystack, regex] = tup_;                                            \
            std::string sub(REGEX);                                                          \
            if (regex)                                                                       \
                return fmt::format("Expected to find regular expression {} /{}/ in {} ({})", \
                                   #REGEX,                                                   \
                                   sub,                                                      \
                                   #BIG_STRING,                                              \
                                   haystack);                                                \
            else                                                                             \
                return fmt::format("Invalid regular expression: {} /{}/", #REGEX, sub);      \
        }()))

namespace mongo::unittest {

bool searchRegex(const pcre::Regex& pattern, const std::string& string);

class Result;

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
    TestAssertionFailureException(std::string file, unsigned line, std::string message);

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
    static constexpr auto comparator() {
        if constexpr (op == ComparisonOp::kEq) {
            return std::equal_to<>{};
        } else if constexpr (op == ComparisonOp::kNe) {
            return std::not_equal_to<>{};
        } else if constexpr (op == ComparisonOp::kLt) {
            return std::less<>{};
        } else if constexpr (op == ComparisonOp::kLe) {
            return std::less_equal<>{};
        } else if constexpr (op == ComparisonOp::kGt) {
            return std::greater<>{};
        } else if constexpr (op == ComparisonOp::kGe) {
            return std::greater_equal<>{};
        }
    }

    static constexpr StringData name() {
        if constexpr (op == ComparisonOp::kEq) {
            return "=="_sd;
        } else if constexpr (op == ComparisonOp::kNe) {
            return "!="_sd;
        } else if constexpr (op == ComparisonOp::kLt) {
            return "<"_sd;
        } else if constexpr (op == ComparisonOp::kLe) {
            return "<="_sd;
        } else if constexpr (op == ComparisonOp::kGt) {
            return ">"_sd;
        } else if constexpr (op == ComparisonOp::kGe) {
            return ">="_sd;
        }
    }

    template <typename A, typename B>
    MONGO_COMPILER_NOINLINE ComparisonAssertion(const char* theFile,
                                                unsigned theLine,
                                                StringData aExpression,
                                                StringData bExpression,
                                                const A& a,
                                                const B& b) {
        if (comparator()(a, b)) {
            return;
        }
        _assertion =
            std::make_unique<TestAssertionFailure>(theFile,
                                                   theLine,
                                                   fmt::format("Expected {1} {0} {2} ({3} {0} {4})",
                                                               name(),
                                                               aExpression,
                                                               bExpression,
                                                               stringify::invoke(a),
                                                               stringify::invoke(b)));
    }

public:
    // Use a single implementation (identical to the templated one) for all string-like types.
    // This is particularly important to avoid making unique instantiations for each length of
    // string literal.
    static ComparisonAssertion make(const char* theFile,
                                    unsigned theLine,
                                    StringData aExpression,
                                    StringData bExpression,
                                    StringData a,
                                    StringData b);

    // Use a single implementation (identical to the templated one) for all pointer and array types.
    // Note: this is selected instead of the StringData overload for char* and string literals
    // because they are supposed to compare pointers, not contents.
    static ComparisonAssertion make(const char* theFile,
                                    unsigned theLine,
                                    StringData aExpression,
                                    StringData bExpression,
                                    const void* a,
                                    const void* b);

    template <typename A, typename B>
    requires(                                                                               //
        !(std::is_convertible_v<A, StringData> && std::is_convertible_v<B, StringData>) &&  //
        !(std::is_pointer_v<A> && std::is_pointer_v<B>) &&                                  //
        !(std::is_array_v<A> && std::is_array_v<B>))                                        //
    static ComparisonAssertion make(const char* theFile,
                                    unsigned theLine,
                                    StringData aExpression,
                                    StringData bExpression,
                                    const A& a,
                                    const B& b) {
        return ComparisonAssertion(theFile, theLine, aExpression, bExpression, a, b);
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

// Explicit instantiation of ComparisonAssertion ctor and factory, for "A OP B".
#define TEMPLATE_COMPARISON_ASSERTION_CTOR_A_OP_B(EXTERN, OP, A, B)             \
    EXTERN template ComparisonAssertion<ComparisonOp::OP>::ComparisonAssertion( \
        const char*, unsigned, StringData, StringData, const A&, const B&);     \
    EXTERN template ComparisonAssertion<ComparisonOp::OP>                       \
    ComparisonAssertion<ComparisonOp::OP>::make(                                \
        const char*, unsigned, StringData, StringData, const A&, const B&);

// Explicit instantiation of ComparisonAssertion ctor and factory for a pair of types.
#define TEMPLATE_COMPARISON_ASSERTION_CTOR_SYMMETRIC(EXTERN, OP, A, B) \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_A_OP_B(EXTERN, OP, A, B)        \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_A_OP_B(EXTERN, OP, B, A)

// Explicit instantiation of ComparisonAssertion ctor and factory for a single type.
#define TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(EXTERN, OP, T) \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_A_OP_B(EXTERN, OP, T, T)

// Call with `extern` to declace extern instantiations, and with no args to explicitly instantiate.
#define INSTANTIATE_COMPARISON_ASSERTION_CTORS(...)                                             \
    __VA_ARGS__ template class ComparisonAssertion<ComparisonOp::kEq>;                          \
    __VA_ARGS__ template class ComparisonAssertion<ComparisonOp::kNe>;                          \
    __VA_ARGS__ template class ComparisonAssertion<ComparisonOp::kGt>;                          \
    __VA_ARGS__ template class ComparisonAssertion<ComparisonOp::kGe>;                          \
    __VA_ARGS__ template class ComparisonAssertion<ComparisonOp::kLt>;                          \
    __VA_ARGS__ template class ComparisonAssertion<ComparisonOp::kLe>;                          \
                                                                                                \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, int)                         \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, long)                        \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, long long)                   \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, unsigned int)                \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, unsigned long)               \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, unsigned long long)          \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, bool)                        \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, double)                      \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, OID)                         \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, BSONType)                    \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, Timestamp)                   \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, Date_t)                      \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, Status)                      \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kEq, ErrorCodes::Error)           \
                                                                                                \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_SYMMETRIC(__VA_ARGS__, kEq, int, long)                   \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_SYMMETRIC(__VA_ARGS__, kEq, int, long long)              \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_SYMMETRIC(__VA_ARGS__, kEq, long, long long)             \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_SYMMETRIC(__VA_ARGS__, kEq, unsigned int, unsigned long) \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_SYMMETRIC(__VA_ARGS__, kEq, Status, ErrorCodes::Error)   \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_SYMMETRIC(__VA_ARGS__, kEq, ErrorCodes::Error, int)      \
                                                                                                \
    /* These are the only types that are often used with ASSERT_NE*/                            \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kNe, Status)                      \
    TEMPLATE_COMPARISON_ASSERTION_CTOR_REFLEXIVE(__VA_ARGS__, kNe, unsigned long)

// Declare that these definitions will be provided in unittest.cpp.
INSTANTIATE_COMPARISON_ASSERTION_CTORS(extern);

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
 * BSON comparison utility macro. Do not use directly.
 */
#define ASSERT_BSON_COMPARISON(NAME, a, b, astr, bstr) \
    ::mongo::unittest::assertComparison_##NAME(__FILE__, __LINE__, astr, bstr, a, b)

/**
 * Use to compare two instances of type BSONObj under the default comparator in unit tests.
 */
#define ASSERT_BSONOBJ_EQ(a, b) ASSERT_BSON_COMPARISON(BSONObjEQ, a, b, #a, #b)
#define ASSERT_BSONOBJ_LT(a, b) ASSERT_BSON_COMPARISON(BSONObjLT, a, b, #a, #b)
#define ASSERT_BSONOBJ_LTE(a, b) ASSERT_BSON_COMPARISON(BSONObjLTE, a, b, #a, #b)
#define ASSERT_BSONOBJ_GT(a, b) ASSERT_BSON_COMPARISON(BSONObjGT, a, b, #a, #b)
#define ASSERT_BSONOBJ_GTE(a, b) ASSERT_BSON_COMPARISON(BSONObjGTE, a, b, #a, #b)
#define ASSERT_BSONOBJ_NE(a, b) ASSERT_BSON_COMPARISON(BSONObjNE, a, b, #a, #b)

/**
 * Use to compare two instances of type BSONObj with unordered fields in unit tests.
 */
#define ASSERT_BSONOBJ_EQ_UNORDERED(a, b) ASSERT_BSON_COMPARISON(BSONObjEQ_UNORDERED, a, b, #a, #b)
#define ASSERT_BSONOBJ_LT_UNORDERED(a, b) ASSERT_BSON_COMPARISON(BSONObjLT_UNORDERED, a, b, #a, #b)
#define ASSERT_BSONOBJ_LTE_UNORDERED(a, b) \
    ASSERT_BSON_COMPARISON(BSONObjLTE_UNORDERED, a, b, #a, #b)
#define ASSERT_BSONOBJ_GT_UNORDERED(a, b) ASSERT_BSON_COMPARISON(BSONObjGT_UNORDERED, a, b, #a, #b)
#define ASSERT_BSONOBJ_GTE_UNORDERED(a, b) \
    ASSERT_BSON_COMPARISON(BSONObjGTE_UNORDERED, a, b, #a, #b)
#define ASSERT_BSONOBJ_NE_UNORDERED(a, b) ASSERT_BSON_COMPARISON(BSONObjNE_UNORDERED, a, b, #a, #b)

/**
 * Use to compare two instances of type BSONElement under the default comparator in unit tests.
 */
#define ASSERT_BSONELT_EQ(a, b) ASSERT_BSON_COMPARISON(BSONElementEQ, a, b, #a, #b)
#define ASSERT_BSONELT_LT(a, b) ASSERT_BSON_COMPARISON(BSONElementLT, a, b, #a, #b)
#define ASSERT_BSONELT_LTE(a, b) ASSERT_BSON_COMPARISON(BSONElementLTE, a, b, #a, #b)
#define ASSERT_BSONELT_GT(a, b) ASSERT_BSON_COMPARISON(BSONElementGT, a, b, #a, #b)
#define ASSERT_BSONELT_GTE(a, b) ASSERT_BSON_COMPARISON(BSONElementGTE, a, b, #a, #b)
#define ASSERT_BSONELT_NE(a, b) ASSERT_BSON_COMPARISON(BSONElementNE, a, b, #a, #b)

#define ASSERT_BSONOBJ_BINARY_EQ(a, b) \
    ::mongo::unittest::assertComparison_BSONObjBINARY_EQ(__FILE__, __LINE__, #a, #b, a, b)

#define DECLARE_BSON_CMP_FUNC(BSONTYPE, NAME)                          \
    void assertComparison_##BSONTYPE##NAME(const std::string& theFile, \
                                           unsigned theLine,           \
                                           StringData aExpression,     \
                                           StringData bExpression,     \
                                           const BSONTYPE& aValue,     \
                                           const BSONTYPE& bValue);

DECLARE_BSON_CMP_FUNC(BSONObj, EQ);
DECLARE_BSON_CMP_FUNC(BSONObj, LT);
DECLARE_BSON_CMP_FUNC(BSONObj, LTE);
DECLARE_BSON_CMP_FUNC(BSONObj, GT);
DECLARE_BSON_CMP_FUNC(BSONObj, GTE);
DECLARE_BSON_CMP_FUNC(BSONObj, NE);

DECLARE_BSON_CMP_FUNC(BSONObj, EQ_UNORDERED);
DECLARE_BSON_CMP_FUNC(BSONObj, LT_UNORDERED);
DECLARE_BSON_CMP_FUNC(BSONObj, LTE_UNORDERED);
DECLARE_BSON_CMP_FUNC(BSONObj, GT_UNORDERED);
DECLARE_BSON_CMP_FUNC(BSONObj, GTE_UNORDERED);
DECLARE_BSON_CMP_FUNC(BSONObj, NE_UNORDERED);

DECLARE_BSON_CMP_FUNC(BSONObj, BINARY_EQ);

DECLARE_BSON_CMP_FUNC(BSONElement, EQ);
DECLARE_BSON_CMP_FUNC(BSONElement, LT);
DECLARE_BSON_CMP_FUNC(BSONElement, LTE);
DECLARE_BSON_CMP_FUNC(BSONElement, GT);
DECLARE_BSON_CMP_FUNC(BSONElement, GTE);
DECLARE_BSON_CMP_FUNC(BSONElement, NE);
#undef DECLARE_BSON_CMP_FUNC

/**
 * Given a BSONObj, return a string that wraps the json form of the BSONObj with
 * `fromjson(R"(<>)")`.
 */
std::string formatJsonStr(const std::string& obj);

#define ASSERT_BSONOBJ_EQ_AUTO(expected, actual)                                     \
    ASSERT(AUTO_UPDATE_HELPER(::mongo::unittest::formatJsonStr(expected),            \
                              ::mongo::unittest::formatJsonStr(actual.jsonString()), \
                              false))

/**
 * Computes a difference between the expected and actual formatted output and outputs it to the
 * provide stream instance. Used to display difference between expected and actual format for
 * auto-update macros. It is exposed in the header here for testability.
 */
void outputDiff(std::ostream& os,
                const std::vector<std::string>& expFormatted,
                const std::vector<std::string>& actualFormatted,
                size_t startLineNumber);

bool handleAutoUpdate(const std::string& expected,
                      const std::string& actual,
                      const std::string& fileName,
                      size_t lineNumber,
                      bool needsEscaping);

bool expandNoPlanMacro(const std::string& fileName, size_t lineNumber);

void updateDelta(const std::string& fileName, size_t lineNumber, int64_t delta);

void expandActualPlan(const SourceLocation& location, const std::string& actual);

// Account for maximum line length after linting. We need to indent, add quotes, etc.
static constexpr size_t kAutoUpdateMaxLineLength = 88;

/**
 * Auto update result back in the source file if the assert fails.
 * The expected result must be a multi-line string in the following form:
 *
 * ASSERT_EXPLAIN_V2_AUTO(     // NOLINT
 *       "BinaryOp [Add]\n"
 *       "|   Const [2]\n"
 *       "Const [1]\n",
 *       tree);
 *
 * Limitations:
 *      1. There should not be any comments or other formatting inside the multi-line string
 *      constant other than 'NOLINT'. If we have a single-line constant, the auto-updating will
 *      generate a 'NOLINT' at the end of the line.
 *      2. The expression which we are explaining ('tree' in the example above) must fit on a single
 *      line.
 *      3. The macro should be indented by 4 spaces.
 */
#define AUTO_UPDATE_HELPER(expected, actual, needsEscaping) \
    ::mongo::unittest::handleAutoUpdate(expected, actual, __FILE__, __LINE__, needsEscaping)

#define ASSERT_STR_EQ_AUTO(expected, actual) ASSERT(AUTO_UPDATE_HELPER(expected, actual, true))

#define ASSERT_NUMBER_EQ_AUTO(expected, actual) \
    ASSERT(AUTO_UPDATE_HELPER(str::stream() << expected, str::stream() << actual, false))
}  // namespace mongo::unittest
