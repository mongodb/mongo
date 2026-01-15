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
#include "mongo/db/exec/mutable_bson/mutable_bson_test_utils.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cmath>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/exception/diagnostic_information.hpp>
#include <fmt/format.h>

MONGO_MOD_PUBLIC;

/**
 * Fail unconditionally, reporting the given message.
 */
#define FAIL(msg) GTEST_FAIL() << (msg)

/**
 * Fails unless "EXPRESSION" is true.
 */
#define ASSERT(EXPRESSION) ASSERT_TRUE(EXPRESSION)

/**
 * Asserts that a Status code is OK.
 * TODO(gtest): Try expressing as `ASSERT_THAT(EXRESSION, IsOk())` which should accept Status and
 * StatusWith (and maybe ErrorCode).
 */
#define ASSERT_OK(EXPRESSION) ASSERT_EQUALS(::mongo::Status::OK(), (EXPRESSION))

/**
 * Asserts that a status code is anything but OK.
 * TODO(gtest) Try expressing as `ASSERT_THAT(EXRESSION, Not(IsOk()))` which should accept Status
 * and StatusWith (and maybe ErrorCode).
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

#define ASSERT_LTE(a, b) ASSERT_LE(a, b)
#define ASSERT_GTE(a, b) ASSERT_GE(a, b)

/**
 * Approximate equality assertion. Useful for comparisons on limited precision floating point
 * values.
 */
#define ASSERT_APPROX_EQUAL(a, b, ABSOLUTE_ERR) ASSERT_NEAR(a, b, ABSOLUTE_ERR)

/**
 * Verify that the evaluation of "EXPRESSION" throws an exception of type EXCEPTION_TYPE.
 *
 * If "EXPRESSION" throws no exception, or one that is neither of type "EXCEPTION_TYPE" nor
 * of a subtype of "EXCEPTION_TYPE", the test is considered a failure and further evaluation
 * halts.
 */
#define ASSERT_THROWS(EXPRESSION, TYPE) ASSERT_THROW((void)(EXPRESSION), TYPE)

/**
 * Verify that the evaluation of "EXPRESSION" does not throw any exceptions.
 *
 * If "EXPRESSION" throws an exception the test is considered a failure and further evaluation
 * halts.
 */
#define ASSERT_DOES_NOT_THROW(EXPRESSION) ASSERT_NO_THROW(EXPRESSION)

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling what() on the thrown exception
 * does not return a string equal to EXPECTED_WHAT.
 * TODO(gtest) Consider using ASSERT_THROWS_WITH_CHECK(...) but with gtest output formatting.
 */
#define ASSERT_THROWS_WHAT(EXPRESSION, EXCEPTION_TYPE, EXPECTED_WHAT) \
    ASSERT_THAT([&] { (void)(EXPRESSION); },                          \
                ::testing::Throws<EXCEPTION_TYPE>(                    \
                    ::testing::Property(&EXCEPTION_TYPE::what, ::testing::StrEq(EXPECTED_WHAT))))

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling getCode() on the thrown exception
 * does not return an error code equal to EXPECTED_CODE.
 */
#define ASSERT_THROWS_CODE(EXPRESSION, EXCEPTION_TYPE, EXPECTED_CODE) \
    ASSERT_THAT([&] { (void)(EXPRESSION); },                          \
                ::testing::Throws<EXCEPTION_TYPE>(                    \
                    ::testing::Property(&EXCEPTION_TYPE::code, EXPECTED_CODE)))

/**
 * Behaves like ASSERT_THROWS, above, but also fails if calling getCode() on the thrown exception
 * does not return an error code equal to EXPECTED_CODE or if calling what() on the thrown exception
 * does not return a string equal to EXPECTED_WHAT.
 */
#define ASSERT_THROWS_CODE_AND_WHAT(EXPRESSION, EXCEPTION_TYPE, EXPECTED_CODE, EXPECTED_WHAT) \
    ASSERT_THAT([&] { (void)(EXPRESSION); },                                                  \
                ::testing::Throws<EXCEPTION_TYPE>(::testing::AllOf(                           \
                    ::testing::Property(&EXCEPTION_TYPE::code, EXPECTED_CODE),                \
                    ::testing::Property(&EXCEPTION_TYPE::what, ::testing::StrEq(EXPECTED_WHAT)))))


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
 * Behaves like ASSERT_THROWS, above, but also calls CHECK(caughtException) which may contain
 * additional assertions.
 */
#define ASSERT_THROWS_WITH_CHECK(EXPRESSION, EXCEPTION_TYPE, CHECK)                \
    if ([&] {                                                                      \
            try {                                                                  \
                (void)(EXPRESSION);                                                \
                return false;                                                      \
            } catch (const EXCEPTION_TYPE& ex) {                                   \
                SCOPED_TRACE(                                                      \
                    fmt::format("\n  expression: {}"                               \
                                "\n  exception: {}",                               \
                                #EXPRESSION,                                       \
                                ::mongo::unittest::describeException(ex)));        \
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

#define ASSERT_STRING_CONTAINS(BIG_STRING, CONTAINS) \
    ASSERT_THAT(BIG_STRING, ::testing::HasSubstr(CONTAINS))

#define ASSERT_STRING_OMITS(BIG_STRING, CONTAINS) \
    ASSERT_THAT(BIG_STRING, ::testing::Not(::testing::HasSubstr(CONTAINS)))

/** TODO(gtest) Consider using a PCRE2 matcher to better match existing behavior. */
#define ASSERT_STRING_SEARCH_REGEX(BIG_STRING, REGEX) \
    ASSERT_THAT(BIG_STRING, ::testing::ContainsRegex(REGEX))

namespace mongo::unittest {

template <typename Ex>
std::string describeException(const Ex& ex) {
    if constexpr (std::is_base_of_v<DBException, Ex>) {
        return ex.toString();
    } else if constexpr (std::is_base_of_v<boost::exception, Ex> ||
                         std::is_base_of_v<std::exception, Ex>) {
        return boost::diagnostic_information(ex, true);
    }
}

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
    MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS                             \
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
