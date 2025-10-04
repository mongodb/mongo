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

/**
 * Unit tests of the unittest framework itself.
 */


#include "mongo/unittest/unittest.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/stringify.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {
namespace mus = mongo::unittest::stringify;

bool containsPattern(const std::string& pattern, const std::string& value) {
    return value.find(pattern) != std::string::npos;
}

#define ASSERT_TEST_FAILS(TEST_STMT) \
    ASSERT_THROWS(                   \
        [&] {                        \
            TEST_STMT;               \
        }(),                         \
        mongo::unittest::TestAssertionFailureException)

#define ASSERT_TEST_FAILS_MATCH(TEST_STMT, PATTERN)     \
    ASSERT_THROWS_WITH_CHECK(                           \
        [&] {                                           \
            TEST_STMT;                                  \
        }(),                                            \
        mongo::unittest::TestAssertionFailureException, \
        ([&](const auto& ex) { ASSERT_STRING_CONTAINS(ex.getMessage(), (PATTERN)); }))

TEST(UnitTestSelfTest, DoNothing) {}

void throwSomething() {
    throw std::exception();
}

TEST(UnitTestSelfTest, TestAssertThrowsSuccess) {
    ASSERT_THROWS(throwSomething(), ::std::exception);
}

class MyException {
public:
    std::string toString() const {
        return what();
    }
    std::string what() const {
        return "whatever";
    }
};

TEST(UnitTestSelfTest, TestAssertThrowsWhatSuccess) {
    ASSERT_THROWS_WHAT(throw MyException(), MyException, "whatever");
}

TEST(UnitTestSelfTest, TestSuccessfulNumericComparisons) {
    ASSERT_EQUALS(1LL, 1.0);
    ASSERT_NOT_EQUALS(1LL, 0.5);
    ASSERT_LESS_THAN(1, 5);
    ASSERT_LESS_THAN_OR_EQUALS(1, 5);
    ASSERT_LESS_THAN_OR_EQUALS(5, 5);
    ASSERT_GREATER_THAN(5, 1);
    ASSERT_GREATER_THAN_OR_EQUALS(5, 1);
    ASSERT_GREATER_THAN_OR_EQUALS(5, 5);
    ASSERT_APPROX_EQUAL(5, 6, 1);
}

TEST(UnitTestSelfTest, TestNumericComparisonFailures) {
    ASSERT_TEST_FAILS(ASSERT_EQUALS(10, 1LL));
    ASSERT_TEST_FAILS(ASSERT_NOT_EQUALS(10, 10LL));
    ASSERT_TEST_FAILS(ASSERT_LESS_THAN(10, 10LL));
    ASSERT_TEST_FAILS(ASSERT_GREATER_THAN(10, 10LL));
    ASSERT_TEST_FAILS(ASSERT_NOT_LESS_THAN(9, 10LL));
    ASSERT_TEST_FAILS(ASSERT_NOT_GREATER_THAN(1, 0LL));
    ASSERT_TEST_FAILS(ASSERT_APPROX_EQUAL(5.0, 6.1, 1));
    if (std::numeric_limits<double>::has_quiet_NaN) {
        ASSERT_TEST_FAILS(ASSERT_APPROX_EQUAL(5, std::numeric_limits<double>::quiet_NaN(), 1));
    }
    if (std::numeric_limits<double>::has_infinity) {
        ASSERT_TEST_FAILS(ASSERT_APPROX_EQUAL(5, std::numeric_limits<double>::infinity(), 1));
    }
}

TEST(UnitTestSelfTest, TestStringComparisons) {
    ASSERT_EQUALS(std::string("hello"), "hello");
    ASSERT_EQUALS("hello", std::string("hello"));

    ASSERT_NOT_EQUALS(std::string("hello"), "good bye!");
    ASSERT_NOT_EQUALS("hello", std::string("good bye!"));

    ASSERT_TEST_FAILS(ASSERT_NOT_EQUALS(std::string("hello"), "hello"));
    ASSERT_TEST_FAILS(ASSERT_NOT_EQUALS("hello", std::string("hello")));

    ASSERT_TEST_FAILS(ASSERT_EQUALS(std::string("hello"), "good bye!"));
    ASSERT_TEST_FAILS(ASSERT_EQUALS("hello", std::string("good bye!")));
}

TEST(UnitTestSelfTest, TestAssertStringContains) {
    ASSERT_STRING_CONTAINS("abcdef", "bcd");
    ASSERT_TEST_FAILS(ASSERT_STRING_CONTAINS("abcdef", "AAA"));
    ASSERT_TEST_FAILS_MATCH(ASSERT_STRING_CONTAINS("abcdef", "AAA") << "XmsgX", "XmsgX");
}

TEST(UnitTestSelfTest, TestAssertStringOmits) {
    ASSERT_STRING_OMITS("abcdef", "AAA");
    ASSERT_TEST_FAILS(ASSERT_STRING_OMITS("abcdef", "bcd"));
    ASSERT_TEST_FAILS_MATCH(ASSERT_STRING_OMITS("abcdef", "bcd") << "XmsgX", "XmsgX");
}

TEST(UnitTestSelfTest, TestAssertStringSearchRegex) {
    ASSERT_STRING_SEARCH_REGEX("abcdef", "^abcdef$");
    ASSERT_STRING_SEARCH_REGEX("abcdef", "cd");
    ASSERT_STRING_SEARCH_REGEX("abcdef", ".*");
    ASSERT_TEST_FAILS(ASSERT_STRING_SEARCH_REGEX("abcdef", "ce"));
    ASSERT_TEST_FAILS(ASSERT_STRING_SEARCH_REGEX("abcdef", ".z."));
    // A regex starting with ? is invalid and shouldn't match.
    ASSERT_TEST_FAILS(ASSERT_STRING_SEARCH_REGEX("?", "?"));
    ASSERT_TEST_FAILS(ASSERT_STRING_SEARCH_REGEX("abcdef", "?.*"));
}

TEST(UnitTestSelfTest, TestStreamingIntoFailures) {
    ASSERT_TEST_FAILS_MATCH(ASSERT_TRUE(false) << "Told you so", "Told you so");
    ASSERT_TEST_FAILS_MATCH(ASSERT(false) << "Told you so", "Told you so");
    ASSERT_TEST_FAILS_MATCH(ASSERT_FALSE(true) << "Told you so", "Told you so");
    ASSERT_TEST_FAILS_MATCH(ASSERT_EQUALS(1, 2) << "Told you so", "Told you so");
    ASSERT_TEST_FAILS_MATCH(FAIL("Foo") << "Told you so", "Told you so");
}

TEST(UnitTestSelfTest, TestNoDoubleEvaluation) {
    int i = 0;
    ASSERT_TEST_FAILS_MATCH(ASSERT_EQ(0, ++i), "(0 == 1)");
}

TEST(UnitTestSelfTest, BSONObjComparisons) {
    auto a = mongo::BSONObjBuilder{}.append("foo", "bar").obj();
    auto b = mongo::BSONObjBuilder{}.append("foo", "baz").obj();
    ASSERT_BSONOBJ_EQ(a, a);
    ASSERT_BSONOBJ_NE(a, b);
    ASSERT_BSONOBJ_LT(a, b);
    ASSERT_BSONOBJ_LTE(a, b);
    ASSERT_BSONOBJ_LTE(b, b);
    ASSERT_BSONOBJ_GT(b, a);
    ASSERT_BSONOBJ_GTE(b, a);
    ASSERT_BSONOBJ_GTE(a, a);

    ASSERT_BSONOBJ_EQ_UNORDERED(a, a);
    ASSERT_BSONOBJ_NE_UNORDERED(a, b);
    ASSERT_BSONOBJ_LT_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(b, b);
    ASSERT_BSONOBJ_GT_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(a, a);
}

TEST(UnitTestSelfTest, BSONObjComparisonsUnordered) {
    auto a = mongo::BSONObjBuilder{}.append("foo", "bar").append("hello", "world").obj();
    auto b = mongo::BSONObjBuilder{}.append("hello", "world").append("foo", "bar").obj();
    ASSERT_BSONOBJ_NE(a, b);
    ASSERT_BSONOBJ_EQ_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(a, b);
    ASSERT_BSONOBJ_LTE_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(b, a);
    ASSERT_BSONOBJ_GTE_UNORDERED(a, b);

    auto c = mongo::BSONObjBuilder{}.append("hello", "world").append("foo", "baz").obj();
    ASSERT_BSONOBJ_NE_UNORDERED(a, c);
    ASSERT_BSONOBJ_NE_UNORDERED(b, c);
    ASSERT_BSONOBJ_LT_UNORDERED(a, c);
    ASSERT_BSONOBJ_LT_UNORDERED(b, c);
    ASSERT_BSONOBJ_GT_UNORDERED(c, b);
    ASSERT_BSONOBJ_GT_UNORDERED(c, a);
}

TEST(UnitTestSelfTest, BSONElementComparisons) {
    auto ao = mongo::BSONObjBuilder{}.append("foo", "bar").obj();
    auto bo = mongo::BSONObjBuilder{}.append("foo", "baz").obj();
    auto a = ao.firstElement();
    auto b = bo.firstElement();
    ASSERT_BSONELT_EQ(a, a);
    ASSERT_BSONELT_NE(a, b);
    ASSERT_BSONELT_LT(a, b);
    ASSERT_BSONELT_LTE(a, a);
    ASSERT_BSONELT_LTE(a, b);
    ASSERT_BSONELT_GT(b, a);
    ASSERT_BSONELT_GTE(b, a);
    ASSERT_BSONELT_GTE(a, a);
}

class UnitTestFormatTest : public mongo::unittest::Test {
public:
    template <template <typename...> class Optional, typename T, typename... As>
    auto mkOptional(As&&... as) {
        return Optional<T>(std::forward<As>(as)...);  // NOLINT
    }

    template <template <typename...> class OptionalTemplate>
    void runFormatOptionalTest() {
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, int>()), "--");
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, std::string>()), "--");
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, int>(123)), " 123");
        ASSERT_EQ(mus::invoke(mkOptional<OptionalTemplate, std::string>("hey")), " hey");
    }

    template <template <typename...> class OptionalTemplate, class None>
    void runEqOptionalTest(None none) {
        ASSERT_EQ(OptionalTemplate<int>{1}, OptionalTemplate<int>{1});  // NOLINT
        ASSERT_NE(OptionalTemplate<int>{1}, OptionalTemplate<int>{2});  // NOLINT
        ASSERT_EQ(OptionalTemplate<int>{}, OptionalTemplate<int>{});    // NOLINT
        ASSERT_EQ(OptionalTemplate<int>{}, none);                       // NOLINT
    }
};

TEST_F(UnitTestFormatTest, FormatBoostOptional) {
    runFormatOptionalTest<boost::optional>();
}

TEST_F(UnitTestFormatTest, EqBoostOptional) {
    runEqOptionalTest<boost::optional>(boost::none);
}

TEST_F(UnitTestFormatTest, FormatStdOptional) {
    runFormatOptionalTest<std::optional>();  // NOLINT
}

TEST_F(UnitTestFormatTest, EqStdOptional) {
    runEqOptionalTest<std::optional>(std::nullopt);  // NOLINT
}

enum class Color { r, g, b };
enum class NamedColor { r, g, b };

inline std::ostream& operator<<(std::ostream& os, const NamedColor& e) {
    return os << std::array{"r", "g", "b"}[static_cast<size_t>(e)];
}

TEST_F(UnitTestFormatTest, FormatEnumClass) {
    ASSERT_STRING_CONTAINS(mus::invoke(Color::r), "Color=0");
    ASSERT_EQ(mus::invoke(NamedColor::r), "r");
    ASSERT_EQ(Color::r, Color::r);
    ASSERT_EQ(NamedColor::r, NamedColor::r);
}

namespace test_extension {
struct X {
    friend std::string stringify_forTest(const X& x) {
        return "X{" + std::to_string(x.x) + "}";
    }

    int x;
};
}  // namespace test_extension

TEST_F(UnitTestFormatTest, FormatCustomized) {
    test_extension::X x{123};
    ASSERT_EQ(mus::invoke(x), "X{123}");
}

DEATH_TEST_REGEX(DeathTestSelfTest, TestDeath, "Invariant failure.*false") {
    invariant(false);
}

class DeathTestSelfTestFixture : public ::mongo::unittest::Test {
public:
    void setUp() override {}
    void tearDown() override {
        LOGV2(24148, "Died in tear-down");
        invariant(false);
    }
};

DEATH_TEST_F(DeathTestSelfTestFixture, DieInTearDown, "Died in tear-down") {}

TEST(UnitTestSelfTest, StackTraceForAssertion) {
    bool threw = false;
    std::string stacktrace;
    try {
        ASSERT_EQ(0, 1);
    } catch (mongo::unittest::TestAssertionFailureException& ae) {
        stacktrace = ae.getStacktrace();
        threw = true;
    }
    ASSERT_TRUE(threw);
    ASSERT_STRING_CONTAINS(stacktrace, "printStackTrace");
}

TEST(UnitTestSelfTest, ComparisonAssertionOverloadResolution) {
    using namespace mongo;

    char xBuf[] = "x";  // Guaranteed different address than "x".
    const char* x = xBuf;

    // At least one StringData, compare contents:
    ASSERT_EQ("x"_sd, "x"_sd);
    ASSERT_EQ("x"_sd, "x");
    ASSERT_EQ("x"_sd, xBuf);
    ASSERT_EQ("x"_sd, x);
    ASSERT_EQ("x", "x"_sd);
    ASSERT_EQ(xBuf, "x"_sd);
    ASSERT_EQ(x, "x"_sd);

    // Otherwise, compare pointers:
    ASSERT_EQ(x, x);
    ASSERT_EQ(xBuf, xBuf);
    ASSERT_EQ(x, xBuf);
    ASSERT_NE("x", xBuf);
    ASSERT_NE("x", x);
    ASSERT_NE(xBuf, "x");
    ASSERT_NE(x, "x");
}

ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckDeclval, typename Char = char, *std::declval<Char>());
ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckEnableIf, bool B = false, std::enable_if_t<B, int>{});

// Uncomment to check that it fails when it is supposed to. Unfortunately we can't check in a test
// that this fails when it is supposed to, only that it passes when it should.
//
// ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckDeclvalFail, typename Char = char,
// *std::declval<Char*>()); ASSERT_DOES_NOT_COMPILE(DoesNotCompileCheckEnableIfFail, bool B = true,
// std::enable_if_t<B, int>{});

}  // namespace
