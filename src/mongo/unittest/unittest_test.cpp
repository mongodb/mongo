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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <functional>
#include <limits>
#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {
namespace stdx = mongo::stdx;

bool containsPattern(const std::string& pattern, const std::string& value) {
    return value.find(pattern) != std::string::npos;
}

#define ASSERT_TEST_FAILS(TEST_STMT) \
    ASSERT_THROWS([&] { TEST_STMT; }(), mongo::unittest::TestAssertionFailureException)

#define ASSERT_TEST_FAILS_MATCH(TEST_STMT, PATTERN)     \
    ASSERT_THROWS_WITH_CHECK(                           \
        [&] { TEST_STMT; }(),                           \
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

TEST(UnitTestSelfTest, TestAssertIdentity) {
    auto intIdentity = [](int x) { return x; };
    ASSERT_IDENTITY(123, intIdentity);
    ASSERT_IDENTITY(123, [](int x) { return x; });
    auto zero = [](auto) { return 0; };
    ASSERT_TEST_FAILS(ASSERT_IDENTITY(1, zero));
    ASSERT_TEST_FAILS_MATCH(ASSERT_IDENTITY(1, zero) << "XmsgX", "XmsgX");
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

TEST(UnitTestSelfTest, BSONObjEQ) {
    ASSERT_BSONOBJ_EQ(BSON("foo"
                           << "bar"),
                      BSON("foo"
                           << "bar"));
}

TEST(UnitTestSelfTest, BSONObjNE) {
    ASSERT_BSONOBJ_NE(BSON("foo"
                           << "bar"),
                      BSON("foo"
                           << "baz"));
}

TEST(UnitTestSelfTest, BSONObjLT) {
    ASSERT_BSONOBJ_LT(BSON("foo"
                           << "bar"),
                      BSON("foo"
                           << "baz"));
}

TEST(UnitTestSelfTest, BSONObjLTE) {
    ASSERT_BSONOBJ_LTE(BSON("foo"
                            << "bar"),
                       BSON("foo"
                            << "baz"));
    ASSERT_BSONOBJ_LTE(BSON("foo"
                            << "bar"),
                       BSON("foo"
                            << "bar"));
}

TEST(UnitTestSelfTest, BSONObjGT) {
    ASSERT_BSONOBJ_GT(BSON("foo"
                           << "baz"),
                      BSON("foo"
                           << "bar"));
}

TEST(UnitTestSelfTest, BSONObjGTE) {
    ASSERT_BSONOBJ_GTE(BSON("foo"
                            << "baz"),
                       BSON("foo"
                            << "bar"));
    ASSERT_BSONOBJ_GTE(BSON("foo"
                            << "bar"),
                       BSON("foo"
                            << "bar"));
}

TEST(UnitTestSelfTest, BSONElementEQ) {
    mongo::BSONObj obj1 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj2 = BSON("foo"
                               << "bar");
    ASSERT_BSONELT_EQ(obj1.firstElement(), obj2.firstElement());
}

TEST(UnitTestSelfTest, BSONElementNE) {
    mongo::BSONObj obj1 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj2 = BSON("foo"
                               << "baz");
    ASSERT_BSONELT_NE(obj1.firstElement(), obj2.firstElement());
}

TEST(UnitTestSelfTest, BSONElementLT) {
    mongo::BSONObj obj1 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj2 = BSON("foo"
                               << "baz");
    ASSERT_BSONELT_LT(obj1.firstElement(), obj2.firstElement());
}

TEST(UnitTestSelfTest, BSONElementLTE) {
    mongo::BSONObj obj1 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj2 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj3 = BSON("foo"
                               << "baz");
    ASSERT_BSONELT_LTE(obj1.firstElement(), obj2.firstElement());
    ASSERT_BSONELT_LTE(obj1.firstElement(), obj3.firstElement());
}

TEST(UnitTestSelfTest, BSONElementGT) {
    mongo::BSONObj obj1 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj2 = BSON("foo"
                               << "baz");
    ASSERT_BSONELT_GT(obj2.firstElement(), obj1.firstElement());
}

TEST(UnitTestSelfTest, BSONElementGTE) {
    mongo::BSONObj obj1 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj2 = BSON("foo"
                               << "bar");
    mongo::BSONObj obj3 = BSON("foo"
                               << "baz");
    ASSERT_BSONELT_GTE(obj3.firstElement(), obj2.firstElement());
    ASSERT_BSONELT_GTE(obj2.firstElement(), obj1.firstElement());
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

ASSERT_DOES_NOT_COMPILE(typename Char = char, *std::declval<Char>());
ASSERT_DOES_NOT_COMPILE(bool B = false, std::enable_if_t<B, int>{});

// Uncomment to check that it fails when it is supposed to. Unfortunately we can't check in a test
// that this fails when it is supposed to, only that it passes when it should.
//
// ASSERT_DOES_NOT_COMPILE(typename Char = char, *std::declval<Char*>());
// ASSERT_DOES_NOT_COMPILE(bool B = true, std::enable_if_t<B, int>{});

}  // namespace
