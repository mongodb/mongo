/**
*    Copyright (C) 2012 MongoDB, Inc.
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

/**
 * Unit tests of the unittest framework itself.
 */

#include "mongo/platform/basic.h"

#include <limits>
#include <string>

#include "mongo/stdx/functional.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {
namespace stdx = mongo::stdx;

bool containsPattern(const std::string& pattern, const std::string& value) {
    return value.find(pattern) != std::string::npos;
}

#define ASSERT_TEST_FAILS(TEST_STMT) \
    ASSERT_THROWS(TEST_STMT, mongo::unittest::TestAssertionFailureException)

#define ASSERT_TEST_FAILS_MATCH(TEST_STMT, PATTERN)                                        \
    ASSERT_THROWS_PRED(                                                                    \
        TEST_STMT,                                                                         \
        mongo::unittest::TestAssertionFailureException,                                    \
        stdx::bind(containsPattern,                                                        \
                   PATTERN,                                                                \
                   stdx::bind(&mongo::unittest::TestAssertionFailureException::getMessage, \
                              stdx::placeholders::_1)))

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

DEATH_TEST(DeathTestSelfTest, TestDeath, "Invariant failure false") {
    invariant(false);
}

class DeathTestSelfTestFixture : public ::mongo::unittest::Test {
public:
    void setUp() override {}
    void tearDown() override {
        mongo::unittest::log() << "Died in tear-down";
        invariant(false);
    }
};

DEATH_TEST_F(DeathTestSelfTestFixture, DieInTearDown, "Died in tear-down") {}
}  // namespace
