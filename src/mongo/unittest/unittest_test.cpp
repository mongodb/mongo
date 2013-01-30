/**
 * Copyright (C) 2012 10gen Inc.
 */

/**
 * Unit tests of the unittest framework itself.
 */

#include "mongo/unittest/unittest.h"

#include <limits>
#include <string>

namespace {

#define ASSERT_TEST_FAILS(TEST_EXPR)                                    \
    ASSERT_THROWS((TEST_EXPR), mongo::unittest::TestAssertionFailureException)

    TEST(UnitTestSelfTest, DoNothing) {
    }

    void throwSomething() { throw std::exception(); }

    TEST(UnitTestSelfTest, TestAssertThrowsSuccess) {
        ASSERT_THROWS(throwSomething(), ::std::exception);
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

}  // namespace
